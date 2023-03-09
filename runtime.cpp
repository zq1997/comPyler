#include <Python.h>
#undef HAVE_STD_ATOMIC
#include <opcode.h>
#include <frameobject.h>
#include <structmember.h>
#include <internal/pycore_pystate.h>
#include <internal/pycore_pyerrors.h>
#include <internal/pycore_abstract.h>
#include <internal/pycore_object.h>
#include <internal/pycore_interp.h>

#include "runtime.h"
#include "ported/interface.h"
#include "translated_result.h"
#include "general_utilities.h"

extern "C" {
PyObject *import_name(PyThreadState *, PyFrameObject *, PyObject *, PyObject *, PyObject *);
PyObject *import_from(PyThreadState *, PyObject *, PyObject *);
int import_all_from(PyThreadState *, PyObject *, PyObject *);
int do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);
void format_awaitable_error(PyThreadState *, PyTypeObject *, int, int);
}


static void restoreExcInfo(PyThreadState *tstate, PyObject **exc_triple) {
    auto exc_info = *tstate->exc_info;
    tstate->exc_info->exc_type = exc_triple[2];
    tstate->exc_info->exc_value = exc_triple[1];
    tstate->exc_info->exc_traceback = exc_triple[0];
    Py_XDECREF(exc_info.exc_type);
    Py_XDECREF(exc_info.exc_value);
    Py_XDECREF(exc_info.exc_traceback);
}

static void clearStack(PyFrameObject *f, int &height, int until_height) {
    auto stack = f->f_valuestack;
    while (height > until_height) {
        Py_XDECREF(stack[--height]);
    }
}

[[noreturn]] static void unwindFrame(PyThreadState *tstate, int stack_height) {
    assert(tstate->frame->f_stackdepth == -1);
    auto cframe = static_cast<ExtendedCFrame *>(tstate->cframe);
    auto f = tstate->frame;
    assert(stack_height >= 0);
    assert(_PyErr_Occurred(tstate));

    f->f_state = FRAME_UNWINDING;

    while (f->f_iblock > 0) {
        /* Pop the current block. */
        auto b = PyFrame_BlockPop(f);
        if (b->b_type == EXCEPT_HANDLER) {
            clearStack(f, stack_height, b->b_level + 3);
            restoreExcInfo(tstate, &f->f_valuestack[stack_height -= 3]);
            continue;
        }

        clearStack(f, stack_height, b->b_level);
        assert(b->b_type == SETUP_FINALLY);
        auto handler = b->b_handler;
        auto exc_info = tstate->exc_info;
        /* Beware, this invalidates all b->b_* fields */
        PyFrame_BlockSetup(f, EXCEPT_HANDLER, f->f_lasti, stack_height);
        f->f_valuestack[stack_height++] = exc_info->exc_traceback;
        f->f_valuestack[stack_height++] = exc_info->exc_value;
        f->f_valuestack[stack_height++] = exc_info->exc_type ? exc_info->exc_type : Py_NewRef(Py_None);
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        /* Make the raw exception data available to the handler,
         * so a program can emulate the Python main loop. */
        _PyErr_NormalizeException(tstate, &exc, &val, &tb);
        auto non_null_tb = tb ? tb : Py_None;
        PyException_SetTraceback(val, non_null_tb);
        exc_info->exc_type = exc;
        exc_info->exc_value = val;
        exc_info->exc_traceback = tb;
        f->f_valuestack[stack_height++] = Py_NewRef(non_null_tb);
        f->f_valuestack[stack_height++] = Py_NewRef(val);
        f->f_valuestack[stack_height++] = Py_NewRef(exc);
        cframe->handler = cframe->translated_result->calcPC(handler);
        f->f_state = FRAME_EXECUTING;
        longjmp(cframe->frame_jmp_buf, 1);
    }

    clearStack(f, stack_height, 0);
    f->f_stackdepth = 0;
    f->f_state = FRAME_RAISED;
    longjmp(cframe->frame_jmp_buf, -1);
}

static auto getThreadState() { return _PyThreadState_GET(); }

[[noreturn]] static void gotoErrorHandler(PyThreadState *tstate) {
    assert(_PyErr_Occurred(tstate));
    auto cframe = static_cast<ExtendedCFrame *>(tstate->cframe);
    int stack_height;
    auto f = tstate->frame;
    if (f->f_code->co_stacksize <= UINT_LEAST8_MAX) {
        stack_height = reinterpret_cast<uint_least8_t *>(cframe->translated_result->stack_height_arr)[f->f_lasti];
    } else {
        stack_height = reinterpret_cast<uint_least16_t *>(cframe->translated_result->stack_height_arr)[f->f_lasti];
    }
    PyTraceBack_Here(f);
    /* Note: tracefunc support here */
    unwindFrame(tstate, stack_height);
}

[[noreturn]] static void gotoErrorHandler() { gotoErrorHandler(getThreadState()); }

static void gotoErrorHandlerIf(bool cond, auto... args) {
    if (cond) [[unlikely]] {
        gotoErrorHandler(args...);
    }
}

static void gotoErrorHandlerIfErrorOccurred(PyThreadState *tstate) {
    if (_PyErr_Occurred(tstate)) {
        gotoErrorHandler(tstate);
    }
}

static void gotoErrorHandlerIfErrorOccurred() { gotoErrorHandlerIfErrorOccurred(getThreadState()); }

static void formatError(PyThreadState *tstate, PyObject *exception, auto value, auto... args) {
    if constexpr (std::is_same_v<decltype(value), const char *>) {
        if constexpr (!sizeof...(args)) {
            _PyErr_SetString(tstate, exception, value);
        } else {
            _PyErr_Format(tstate, exception, value, args...);
        }
    } else {
        static_assert(!sizeof...(args));
        if constexpr (std::is_same_v<decltype(value), _Py_Identifier *>) {
            auto error_obj = _PyUnicode_FromId(value);
            gotoErrorHandlerIf(!error_obj, tstate);
            _PyErr_SetObject(tstate, exception, error_obj);
        } else {
            static_assert(std::is_same_v<decltype(value), PyObject *>);
            _PyErr_SetObject(tstate, exception, value);
        }
    }
}

static void formatErrorIfNotOccurred(PyThreadState *tstate, auto... args) {
    if (!_PyErr_Occurred(tstate)) {
        formatError(tstate, args...);
    }
}

[[noreturn]] static void formatErrorAndGotoHandler(PyThreadState *tstate, auto... args) {
    assert(!_PyErr_Occurred(tstate));
    formatError(tstate, args...);
    gotoErrorHandler(tstate);
}

[[noreturn]] static void formatErrorAndGotoHandler(PyObject *exception, const char *format, auto... args) {
    formatErrorAndGotoHandler(getThreadState(), exception, format, args...);
}

static void formatErrorAndGotoHandlerIf(bool cond, auto... more_args) {
    if (cond) [[unlikely]] {
        formatErrorAndGotoHandler(more_args...);
    }
}

void handleEvalBreaker() {
    auto tstate = getThreadState();
    gotoErrorHandlerIf(eval_frame_handle_pending(tstate), tstate);
}

void raiseUndefinedName(PyThreadState *tstate, PyObject *name, bool is_free_var) {
    _PyErr_Format(tstate, PyExc_NameError, is_free_var ?
                    "free variable '%.200U' referenced before assignment in enclosing scope" :
                    "name '%.200U' is not defined",
            name);
    PyObject *type, *value, *traceback;
    _PyErr_Fetch(tstate, &type, &value, &traceback);
    _PyErr_NormalizeException(tstate, &type, &value, &traceback);
    if (PyErr_GivenExceptionMatches(value, PyExc_NameError)) {
        _Py_IDENTIFIER(name);
        _PyObject_SetAttrId(value, &PyId_name, name);
    }
    _PyErr_Restore(tstate, type, value, traceback);
}

[[noreturn]] void raiseUnboundError() {
    auto tstate = getThreadState();
    const auto &raise_undefined_local_error = [=](PyObject *name) {
        _PyErr_Format(tstate, PyExc_UnboundLocalError,
                "local variable '%.200U' referenced before assignment",
                name);
    };

    PyCode py_code{tstate->frame->f_code};
    int opcode = EXTENDED_ARG;
    PyOparg oparg = 0;
    for (auto instr = &py_code.instrData()[tstate->frame->f_lasti]; opcode == EXTENDED_ARG; ++instr) {
        opcode = _Py_OPCODE(*instr);
        oparg = oparg << PyCode::extended_arg_shift | _Py_OPARG(*instr);
    }
    if (opcode == LOAD_FAST || opcode == DELETE_FAST) {
        auto name = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
        raise_undefined_local_error(name);
    } else if (opcode == LOAD_DEREF || opcode == DELETE_DEREF) {
        PyOparg cell_num = PyTuple_GET_SIZE(py_code->co_cellvars);
        if (oparg < cell_num) {
            raise_undefined_local_error(PyTuple_GET_ITEM(py_code->co_cellvars, oparg));
        } else {
            auto name = PyTuple_GET_ITEM(py_code->co_freevars, oparg - cell_num);
            raiseUndefinedName(tstate, name, true);
        }
    } else {
        Py_UNREACHABLE();
    }
    gotoErrorHandler(tstate);
}

void handle_ROT_N(PyObject **values, Py_ssize_t n_lift) {
    auto ptr = &values[n_lift];
    auto top = *ptr;
    do {
        *ptr = *(ptr - 1);
    } while (--ptr != values);
    *ptr = top;
}

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, Py_ssize_t index) {
    auto locals = f->f_locals;
    assert(locals);
    auto co = f->f_code;
    auto free_index = index - PyTuple_GET_SIZE(co->co_cellvars);
    assert(free_index >= 0 && free_index < PyTuple_GET_SIZE(co->co_freevars));
    auto name = PyTuple_GET_ITEM(co->co_freevars, free_index);
    if (PyDict_CheckExact(locals)) {
        auto value = PyDict_GetItemWithError(locals, name);
        if (value) {
            Py_INCREF(value);
            return value;
        } else {
            gotoErrorHandlerIfErrorOccurred();
        }
    } else {
        auto value = PyObject_GetItem(locals, name);
        if (value) {
            return value;
        } else {
            auto tstate = getThreadState();
            gotoErrorHandlerIf(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError), tstate);
            _PyErr_Clear(tstate);
        }
    }
    auto value = PyCell_GET(f->f_localsplus[f->f_code->co_nlocals + index]);
    if (value) {
        Py_INCREF(value);
        return value;
    }
    auto tstate = getThreadState();
    raiseUndefinedName(tstate, name, true);
    gotoErrorHandler(tstate);
}

PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyObject *name, _PyOpcache *co_opcache) {
    assert(PyDict_CheckExact(f->f_globals));

    if (PyDict_CheckExact(f->f_builtins)) {
        auto globals = reinterpret_cast<PyDictObject *>(f->f_globals);
        auto builtins = reinterpret_cast<PyDictObject *>(f->f_builtins);
        auto &lg = co_opcache->u.lg;
        if (co_opcache->optimized > 0) {
            if (lg.globals_ver == globals->ma_version_tag && lg.builtins_ver == builtins->ma_version_tag) {
                return Py_NewRef(lg.ptr);
            }
        }
        auto v = _PyDict_LoadGlobal(globals, builtins, name);
        if (!v) {
            auto tstate = getThreadState();
            if (!_PyErr_Occurred(tstate)) {
                raiseUndefinedName(tstate, name);
            }
            gotoErrorHandler(tstate);
        }

        co_opcache->optimized = 1;
        lg.globals_ver = globals->ma_version_tag;
        lg.builtins_ver = builtins->ma_version_tag;
        lg.ptr = v;
        return Py_NewRef(v);
    } else {
        if (auto v = PyDict_GetItemWithError(f->f_globals, name)) {
            return Py_NewRef(v);
        }
        auto tstate = getThreadState();
        gotoErrorHandlerIfErrorOccurred(tstate);
        if (auto v = PyObject_GetItem(f->f_builtins, name)) {
            return v;
        }
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
        gotoErrorHandler(tstate);
    }
}

void handle_STORE_GLOBAL(PyFrameObject *f, PyObject *name, PyObject *value) {
    auto err = PyDict_SetItem(f->f_globals, name, value);
    gotoErrorHandlerIf(err);
}

void handle_DELETE_GLOBAL(PyFrameObject *f, PyObject *name) {
    if (PyDict_DelItem(f->f_globals, name)) {
        auto tstate = getThreadState();
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
        gotoErrorHandler(tstate);
    }
}

PyObject *handle_LOAD_NAME(PyFrameObject *f, PyObject *name) {
    formatErrorAndGotoHandlerIf(!f->f_locals, PyExc_SystemError, "no locals when loading %R", name);

    PyThreadState *tstate;
    if (PyDict_CheckExact(f->f_locals)) {
        if (auto v = PyDict_GetItemWithError(f->f_locals, name)) {
            return Py_NewRef(v);
        } else {
            tstate = getThreadState();
            gotoErrorHandlerIfErrorOccurred(tstate);
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_locals, name)) {
            return v;
        } else {
            tstate = getThreadState();
            gotoErrorHandlerIf(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError), tstate);
            _PyErr_Clear(tstate);
        }
    }

    assert(PyDict_CheckExact(f->f_globals));
    if (auto v = PyDict_GetItemWithError(f->f_globals, name)) {
        return Py_NewRef(v);
    } else {
        gotoErrorHandlerIfErrorOccurred(tstate);
    }

    if (PyDict_CheckExact(f->f_builtins)) {
        if (auto v = PyDict_GetItemWithError(f->f_builtins, name)) {
            return Py_NewRef(v);
        }
        if (!_PyErr_Occurred(tstate)) {
            raiseUndefinedName(tstate, name);
        }
    } else {
        if (auto v = PyObject_GetItem(f->f_builtins, name)) {
            return v;
        }
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            raiseUndefinedName(tstate, name);
        }
    }
    gotoErrorHandler(tstate);
}

void handle_STORE_NAME(PyFrameObject *f, PyObject *name, PyObject *value) {
    formatErrorAndGotoHandlerIf(!f->f_locals, PyExc_SystemError, "no locals found when storing %R", name);
    int err;
    if (PyDict_CheckExact(f->f_locals)) {
        err = PyDict_SetItem(f->f_locals, name, value);
    } else {
        err = PyObject_SetItem(f->f_locals, name, value);
    }
    gotoErrorHandlerIf(err);
}

void handle_DELETE_NAME(PyFrameObject *f, PyObject *name) {
    PyObject *ns = f->f_locals;
    formatErrorAndGotoHandlerIf(!ns, PyExc_SystemError, "no locals when deleting %R", name);
    if (PyObject_DelItem(ns, name)) {
        auto tstate = getThreadState();
        raiseUndefinedName(tstate, name);
        gotoErrorHandler(tstate);
    }
}

PyObject *handle_LOAD_ATTR(PyObject *owner, PyObject *name, PyFrameObject *f, _PyOpcache *co_opcache) {
    /* It is a bug that CPython use char, since char may be unsigned */
    auto &optimized = reinterpret_cast<signed char &>(co_opcache->optimized);

    const auto &update_opcache = [&](auto type, auto hint, auto tp_version_tag) {
        if (!optimized) {
            optimized = 20; // OPCODE_CACHE_MAX_TRIES
        }
        auto &la = co_opcache->u.la;
        la.type = type;
        la.hint = hint;
        la.tp_version_tag = tp_version_tag;
    };
    const auto &deopt_opcache = [&]() {
        optimized = -1;
        co_opcache = nullptr;
    };
    const auto &maybe_deopt_opcache = [&]() {
        if (--optimized == 0) {
            deopt_opcache();
        }
    };

    auto type = Py_TYPE(owner);
    if (optimized >= 0 && PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
        if (optimized > 0) {
            auto &la = co_opcache->u.la;
            if (la.type == type && la.tp_version_tag == type->tp_version_tag) {
                if (la.hint < -1) {
                    auto *res = *reinterpret_cast<PyObject **>(reinterpret_cast<char *>(owner) + ~la.hint);
                    if (res) {
                        return Py_NewRef(res);
                    } else {
                        // Make sure it jump to slow path directly
                        co_opcache = nullptr;
                    }
                } else {
                    assert(type->tp_dict);
                    assert(type->tp_dictoffset > 0);
                    auto dict = *reinterpret_cast<PyObject **>(reinterpret_cast<char *>(owner) + type->tp_dictoffset);
                    if (dict && PyDict_CheckExact(dict)) {
                        Py_INCREF(dict);
                        PyObject *res = nullptr;
                        auto hint = _PyDict_GetItemHint(reinterpret_cast<PyDictObject *>(dict), name, la.hint, &res);
                        if (res) {
                            assert(hint >= 0);
                            Py_INCREF(res);
                            Py_DECREF(dict);
                            if (la.hint != hint) {
                                la.hint = hint;
                                maybe_deopt_opcache();
                            }
                            return res;
                        }
                    }
                    deopt_opcache();
                }
            } else {
                maybe_deopt_opcache();
            }
        }

        if (co_opcache && type->tp_getattro == PyObject_GenericGetAttr) {
            gotoErrorHandlerIf(!type->tp_dict && !PyType_Ready(type));
            if (auto descr = _PyType_Lookup(type, name)) {
                if (Py_TYPE(descr) == &PyMemberDescr_Type) {
                    auto dmem = reinterpret_cast<PyMemberDescrObject *>(descr)->d_member;
                    if (dmem->type == T_OBJECT_EX) {
                        auto offset = dmem->offset;
                        assert(offset > 0);
                        auto *res = *reinterpret_cast<PyObject **>(reinterpret_cast<char *>(owner) + offset);
                        if (res) {
                            Py_INCREF(res);
                            update_opcache(type, ~offset, type->tp_version_tag);
                            return res;
                        } else {
                            // Prevent deopt_opcache()
                            co_opcache = nullptr;
                        }
                    }
                }
            } else if (type->tp_dictoffset > 0) {
                auto dict = *reinterpret_cast<PyObject **>(reinterpret_cast<char *>(owner) + type->tp_dictoffset);
                if (dict && PyDict_CheckExact(dict)) {
                    Py_INCREF(dict);
                    PyObject *res = nullptr;
                    auto hint = _PyDict_GetItemHint(reinterpret_cast<PyDictObject *>(dict), name, -1, &res);
                    if (res) {
                        assert(hint >= 0);
                        Py_INCREF(res);
                        Py_DECREF(dict);
                        update_opcache(type, hint, type->tp_version_tag);
                        return res;
                    } else {
                        Py_DECREF(dict);
                        PyErr_Clear();
                        co_opcache = nullptr;
                    }
                }
            }
        }
        if (co_opcache) {
            deopt_opcache();
        }
    }
    auto value = PyObject_GetAttr(owner, name);
    gotoErrorHandlerIf(!value);
    return value;
}

void handle_LOAD_METHOD(PyObject *name, PyObject **sp) {
    PyObject *obj = sp[0];
    PyObject *meth = nullptr;
    int meth_found = _PyObject_GetMethod(obj, name, &meth);
    gotoErrorHandlerIf(!meth);
    if (meth_found) {
        sp[0] = meth;
        sp[1] = obj;
    } else {
        sp[0] = nullptr;
        sp[1] = meth;
        Py_DECREF(obj);
    }
}

void handle_STORE_ATTR(PyObject *owner, PyObject *name, PyObject *value) {
    gotoErrorHandlerIf(PyObject_SetAttr(owner, name, value));
}

// 加速tuple list dict
PyObject *handle_BINARY_SUBSCR(PyObject *container, PyObject *sub) {
    auto value = PyObject_GetItem(container, sub);
    gotoErrorHandlerIf(!value);
    return value;
}

void handle_STORE_SUBSCR(PyObject *container, PyObject *sub, PyObject *value) {
    auto err = PyObject_SetItem(container, sub, value);
    gotoErrorHandlerIf(err);
}

void handle_DELETE_SUBSCR(PyObject *container, PyObject *sub) {
    auto err = PyObject_DelItem(container, sub);
    gotoErrorHandlerIf(err);
}

template <typename T>
static const char *getSlotSign(T PyNumberMethods::* op_slot) {
    PyNumberMethods dummy;
    switch (reinterpret_cast<char *>(&(dummy.*op_slot)) - reinterpret_cast<char *>(&dummy)) {
    case offsetof(PyNumberMethods, nb_positive):
        return "+";
    case offsetof(PyNumberMethods, nb_negative):
        return "-";
    case offsetof(PyNumberMethods, nb_invert):
        return "~";
    case offsetof(PyNumberMethods, nb_add):
        return "+";
    case offsetof(PyNumberMethods, nb_inplace_add):
        return "+=";
    case offsetof(PyNumberMethods, nb_subtract):
        return "-";
    case offsetof(PyNumberMethods, nb_inplace_subtract):
        return "-=";
    case offsetof(PyNumberMethods, nb_multiply):
        return "*";
    case offsetof(PyNumberMethods, nb_inplace_multiply):
        return "*=";
    case offsetof(PyNumberMethods, nb_floor_divide):
        return "//";
    case offsetof(PyNumberMethods, nb_inplace_floor_divide):
        return "//=";
    case offsetof(PyNumberMethods, nb_true_divide):
        return "/";
    case offsetof(PyNumberMethods, nb_inplace_true_divide):
        return "/=";
    case offsetof(PyNumberMethods, nb_remainder):
        return "%";
    case offsetof(PyNumberMethods, nb_inplace_remainder):
        return "%=";
    case offsetof(PyNumberMethods, nb_power):
        return "** or pow()";
    case offsetof(PyNumberMethods, nb_inplace_power):
        return "**=";
    case offsetof(PyNumberMethods, nb_matrix_multiply):
        return "@";
    case offsetof(PyNumberMethods, nb_inplace_matrix_multiply):
        return "@=";
    case offsetof(PyNumberMethods, nb_lshift):
        return "<<";
    case offsetof(PyNumberMethods, nb_inplace_lshift):
        return "<<=";
    case offsetof(PyNumberMethods, nb_rshift):
        return ">>";
    case offsetof(PyNumberMethods, nb_inplace_rshift):
        return ">>=";
    case offsetof(PyNumberMethods, nb_and):
        return "&";
    case offsetof(PyNumberMethods, nb_inplace_and):
        return "&=";
    case offsetof(PyNumberMethods, nb_or):
        return "|";
    case offsetof(PyNumberMethods, nb_inplace_or):
        return "|=";
    case offsetof(PyNumberMethods, nb_xor):
        return "^";
    case offsetof(PyNumberMethods, nb_inplace_xor):
        return "^=";
    default:
        Py_UNREACHABLE();
    }
}

template <typename T>
static auto checkSlotCallResult(PyObject *result, PyObject *obj, T PyNumberMethods::* op_slot) {
#ifndef NDEBUG
    PyThreadState *tstate = getThreadState();
    auto slot_sign = getSlotSign(op_slot);
    if (!result ^ !!_PyErr_Occurred(tstate)) {
        _Py_FatalErrorFormat(__func__, "Slot %s of type %s %s an exception set",
                slot_sign, result ? "succeeded with" : "failed without", Py_TYPE(obj)->tp_name);
    }
#endif
    gotoErrorHandlerIf(!result);
    return result;
}

template <unaryfunc PyNumberMethods::*P, const char *slot_name>
PyObject *handle_UNARY(PyObject *value) {
    auto type = Py_TYPE(value);
    auto *m = type->tp_as_number;
    formatErrorAndGotoHandlerIf(!m || !(m->*P), PyExc_TypeError,
            "bad operand type for unary %s: '%.200s'", getSlotSign(P), type->tp_name);
    return checkSlotCallResult((m->*P)(value), value, P);
}

PyObject *handle_UNARY_NOT(PyObject *value) {
    // castPyObjectToBool does not have the fast path for handling Py_True/Py_False
    if (value == Py_True) {
        return Py_NewRef(Py_False);
    }
    if (value == Py_False) {
        return Py_NewRef(Py_True);
    }
    return Py_NewRef(castPyObjectToBool(value) ? Py_False : Py_True);
}

PyObject *handle_UNARY_POSITIVE(PyObject *value) {
    static constexpr char slot[]{"__pos__"};
    return handle_UNARY<&PyNumberMethods::nb_positive, slot>(value);
}

PyObject *handle_UNARY_NEGATIVE(PyObject *value) {
    static constexpr char slot[]{"__neg__"};
    return handle_UNARY<&PyNumberMethods::nb_negative, slot>(value);
}

PyObject *handle_UNARY_INVERT(PyObject *value) {
    static constexpr char slot[]{"__invert__"};
    return handle_UNARY<&PyNumberMethods::nb_invert, slot>(value);
}

template <typename T>
[[noreturn]] static void raiseBinOpTypeError(PyObject *v, PyObject *w, T op_slot, const char *hint = "") {
    formatErrorAndGotoHandler(PyExc_TypeError,
            "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'%s",
            getSlotSign(op_slot),
            Py_TYPE(v)->tp_name,
            Py_TYPE(w)->tp_name,
            hint);
}

template <bool return_null = false, typename T, typename... Ts>
static PyObject *handleBinary(PyObject *v, PyObject *w, T PyNumberMethods::* op_slot, Ts... more_op_slots) {
    const auto &invoke_slot_func = [&](T func) {
        if constexpr (std::is_same_v<T, ternaryfunc>) {
            return func(v, w, Py_None);
        } else {
            return func(v, w);
        }
    };

    auto type_v = Py_TYPE(v);
    auto type_w = Py_TYPE(w);
    auto slots_v = type_v->tp_as_number;
    auto slots_w = type_w->tp_as_number;
    const auto original_op_slot = op_slot;

    if constexpr (sizeof...(Ts)) {
        static_assert(sizeof...(Ts) == 1);
        if (slots_v && slots_v->*op_slot) {
            auto result = checkSlotCallResult(invoke_slot_func(slots_v->*op_slot), v, original_op_slot);
            if (result != Py_NotImplemented) {
                return result;
            }
            Py_DECREF(result);
        }
        op_slot = std::get<0>(std::tuple{more_op_slots...});
    }

    auto func_v = slots_v ? slots_v->*op_slot : nullptr;
    auto func_w = slots_w ? slots_w->*op_slot : nullptr;
    func_w = func_w == func_v ? nullptr : func_w;

    if (func_v) {
        if (func_w && PyType_IsSubtype(type_w, type_v)) {
            auto result = checkSlotCallResult(invoke_slot_func(func_w), w, original_op_slot);
            if (result != Py_NotImplemented) {
                return result;
            }
            Py_DECREF(result);
            func_w = nullptr;
        }
        auto result = checkSlotCallResult(invoke_slot_func(func_v), v, original_op_slot);
        if (result != Py_NotImplemented) {
            return result;
        }
        Py_DECREF(result);
    }
    if (func_w) {
        auto result = checkSlotCallResult(invoke_slot_func(func_w), w, original_op_slot);
        if (result != Py_NotImplemented) {
            return result;
        }
        Py_DECREF(result);
    }
    if constexpr (return_null) {
        return nullptr;
    } else {
        raiseBinOpTypeError(v, w, original_op_slot);
    }
}

PyObject *handle_BINARY_ADD(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_add;
    if (auto result = handleBinary<true>(v, w, op_slot)) {
        return result;
    }
    auto m = Py_TYPE(v)->tp_as_sequence;
    if (m && m->sq_concat) {
        return checkSlotCallResult((m->sq_concat)(v, w), v, op_slot);
    }
    raiseBinOpTypeError(v, w, op_slot);
}

PyObject *handle_INPLACE_ADD(PyObject *v, PyObject *w) {
    constexpr auto iop_slot = &PyNumberMethods::nb_inplace_add;
    if (auto result = handleBinary<true>(v, w, iop_slot, &PyNumberMethods::nb_add)) {
        return result;
    }
    auto m = Py_TYPE(v)->tp_as_sequence;
    if (m) {
        auto func = m->sq_inplace_concat ? m->sq_inplace_concat : m->sq_concat;
        if (func) {
            return checkSlotCallResult(func(v, w), v, iop_slot);
        }
    }
    raiseBinOpTypeError(v, w, iop_slot);
}

PyObject *handle_BINARY_SUBTRACT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_subtract);
}

PyObject *handle_INPLACE_SUBTRACT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_subtract, &PyNumberMethods::nb_subtract);
}

static PyObject *repeatSequence(PyObject *v, PyObject *w, binaryfunc PyNumberMethods::*op_slot) {
    ssizeargfunc repeat_func = nullptr;
    PyObject *seq;
    PyObject *n;
    auto mv = Py_TYPE(v)->tp_as_sequence;
    auto mw = Py_TYPE(w)->tp_as_sequence;
    if (mv) {
        if (op_slot == &PyNumberMethods::nb_inplace_multiply) {
            repeat_func = mv->sq_inplace_repeat ? mv->sq_inplace_repeat : mv->sq_repeat;
        } else {
            repeat_func = mv->sq_repeat;
        }
        seq = v;
        n = w;
    }
    if (!repeat_func) {
        if (mw && mw->sq_repeat) {
            repeat_func = mw->sq_repeat;
            seq = w;
            n = v;
        } else {
            raiseBinOpTypeError(v, w, op_slot);
        }
    }

    formatErrorAndGotoHandlerIf(!_PyIndex_Check(n), PyExc_TypeError,
            "can't multiply sequence by non-int of type '%.200s'", Py_TYPE(n)->tp_name);
    auto count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
    if (count == -1) {
        gotoErrorHandlerIfErrorOccurred();
    }
    return checkSlotCallResult(repeat_func(seq, count), seq, op_slot);
}

PyObject *handle_BINARY_MULTIPLY(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_multiply;
    auto result = handleBinary<true>(v, w, op_slot);
    if (result) {
        return result;
    }
    return repeatSequence(v, w, op_slot);
}


PyObject *handle_INPLACE_MULTIPLY(PyObject *v, PyObject *w) {
    constexpr auto iop_slot = &PyNumberMethods::nb_inplace_multiply;
    auto result = handleBinary<true>(v, w, iop_slot, &PyNumberMethods::nb_multiply);
    if (result) {
        return result;
    }
    return repeatSequence(v, w, iop_slot);
}

PyObject *handle_BINARY_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_floor_divide);
}

PyObject *handle_INPLACE_FLOOR_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_floor_divide, &PyNumberMethods::nb_floor_divide);
}

PyObject *handle_BINARY_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_true_divide);
}

PyObject *handle_INPLACE_TRUE_DIVIDE(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_true_divide, &PyNumberMethods::nb_true_divide);
}

PyObject *handle_BINARY_MODULO(PyObject *v, PyObject *w) {
    if (PyUnicode_CheckExact(v) && (PyUnicode_CheckExact(w) || !PyUnicode_Check(w))) {
        // fast path
        auto res = PyUnicode_Format(v, w);
        gotoErrorHandlerIf(!res);
        return res;
    } else {
        return handleBinary(v, w, &PyNumberMethods::nb_remainder);
    }
}

PyObject *handle_INPLACE_MODULO(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_remainder, &PyNumberMethods::nb_remainder);
}

PyObject *handle_BINARY_POWER(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_power);
}

PyObject *handle_INPLACE_POWER(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_power, &PyNumberMethods::nb_power);
}

PyObject *handle_BINARY_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_matrix_multiply);
}

PyObject *handle_INPLACE_MATRIX_MULTIPLY(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_matrix_multiply, &PyNumberMethods::nb_matrix_multiply);
}

PyObject *handle_BINARY_LSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_lshift);
}

PyObject *handle_INPLACE_LSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_lshift, &PyNumberMethods::nb_lshift);
}

PyObject *handle_BINARY_RSHIFT(PyObject *v, PyObject *w) {
    constexpr auto op_slot = &PyNumberMethods::nb_rshift;
    auto result = handleBinary<true>(v, w, op_slot);
    if (result) {
        return result;
    }
    auto hint = PyCFunction_CheckExact(v)
            && !strcmp("print", reinterpret_cast<PyCFunctionObject *>(v)->m_ml->ml_name) ?
            " Did you mean \"print(<message>, file=<output_stream>)\"?" : "";
    raiseBinOpTypeError(v, w, op_slot, hint);
}

PyObject *handle_INPLACE_RSHIFT(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_rshift, &PyNumberMethods::nb_rshift);
}

PyObject *handle_BINARY_AND(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_and);
}

PyObject *handle_INPLACE_AND(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_and, &PyNumberMethods::nb_and);
}

PyObject *handle_BINARY_OR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_or);
}

PyObject *handle_INPLACE_OR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_or, &PyNumberMethods::nb_or);
}

PyObject *handle_BINARY_XOR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_xor);
}

PyObject *handle_INPLACE_XOR(PyObject *v, PyObject *w) {
    return handleBinary(v, w, &PyNumberMethods::nb_inplace_xor, &PyNumberMethods::nb_xor);
}

PyObject *handle_COMPARE_OP(PyObject *v, PyObject *w, int op) {
    auto type_v = Py_TYPE(v);
    auto type_w = Py_TYPE(w);
    auto slot_v = type_v->tp_richcompare;
    auto slot_w = type_w->tp_richcompare;

    if (slot_w && type_v != type_w && PyType_IsSubtype(type_w, type_v)) {
        auto res = slot_w(w, v, _Py_SwappedOp[op]);
        gotoErrorHandlerIf(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
        slot_w = nullptr;
    }
    if (slot_v) {
        auto res = slot_v(v, w, op);
        gotoErrorHandlerIf(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
    }
    if (slot_w) {
        auto res = slot_w(w, v, _Py_SwappedOp[op]);
        gotoErrorHandlerIf(!res);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
    }
    if (op == Py_EQ || op == Py_NE) {
        return Py_NewRef((v == w) ^ (op == Py_NE) ? Py_True : Py_False);
    }

    static const char *const op_signs[]{"<", "<=", "==", "!=", ">", ">="};
    formatErrorAndGotoHandler(PyExc_TypeError,
            "'%s' not supported between instances of '%.100s' and '%.100s'",
            op_signs[op],
            type_v->tp_name,
            type_w->tp_name);
}

PyObject *handle_CONTAINS_OP(PyObject *value, PyObject *container, bool invert) {
    auto sqm = Py_TYPE(container)->tp_as_sequence;
    Py_ssize_t res;
    if (sqm && sqm->sq_contains) {
        res = sqm->sq_contains(container, value);
    } else {
        res = _PySequence_IterSearch(container, value, PY_ITERSEARCH_CONTAINS);
    }
    gotoErrorHandlerIf(res < 0);
    auto res_value = invert ^ (res > 0) ? Py_True : Py_False;
    Py_INCREF(res_value);
    return res_value;
}

bool castPyObjectToBool(PyObject *o) {
    if (o == Py_None) {
        return false;
    }
    auto type = Py_TYPE(o);
    Py_ssize_t res;
    if (type->tp_as_number && type->tp_as_number->nb_bool) {
        res = type->tp_as_number->nb_bool(o);
    } else if (type->tp_as_mapping && type->tp_as_mapping->mp_length) {
        res = type->tp_as_mapping->mp_length(o);
    } else if (type->tp_as_sequence && type->tp_as_sequence->sq_length) {
        res = type->tp_as_sequence->sq_length(o);
    } else {
        return true;
    }
    gotoErrorHandlerIf(res < 0);
    return res > 0;
}

PyObject *handle_GET_ITER(PyObject *o) {
    auto type = Py_TYPE(o);
    if (type->tp_iter) {
        auto *res = type->tp_iter(o);
        gotoErrorHandlerIf(!res);
        auto res_type = Py_TYPE(res);
        if (res_type->tp_iternext && res_type->tp_iternext != &_PyObject_NextNotImplemented) {
            return res;
        } else {
            PyErr_Format(PyExc_TypeError, "iter() returned non-iterator of type '%.100s'", res_type->tp_name);
            Py_DECREF(res);
            gotoErrorHandler();
        }
    } else {
        if (!PyDict_Check(o) && type->tp_as_sequence && type->tp_as_sequence->sq_item) {
            auto res = PySeqIter_New(o);
            gotoErrorHandlerIf(!res);
            return res;
        }
        formatErrorAndGotoHandler(PyExc_TypeError, "'%.200s' object is not iterable", type->tp_name);
    }
}

static auto makeFunctionCall(PyObject *func_args[], Py_ssize_t nargs, PyObject *kwnames, Py_ssize_t decref) {
    auto tstate = getThreadState();
    auto ret = _PyObject_VectorcallTstate(tstate, func_args[0], func_args + 1,
            nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    gotoErrorHandlerIf(!ret, tstate);
    do {
        Py_DECREF(func_args[decref]);
    } while (decref--);
    /* Note: tracefunc support here */
    return ret;
}

PyObject *handle_CALL_FUNCTION(PyObject **func_args, Py_ssize_t nargs) {
    return makeFunctionCall(func_args, nargs, nullptr, nargs);
}

PyObject *handle_CALL_METHOD(PyObject **func_args, Py_ssize_t nargs) {
    bool is_meth = func_args[0];
    func_args += !is_meth;
    nargs += is_meth;
    return makeFunctionCall(func_args, nargs, nullptr, nargs);
}

PyObject *handle_CALL_FUNCTION_KW(PyObject **func_args, Py_ssize_t nargs, PyObject *kwnames) {
    return makeFunctionCall(func_args, nargs - PyTuple_GET_SIZE(kwnames), kwnames, nargs);
}

static void formatFunctionCallError(PyThreadState *tstate, PyObject *func, const char *error, auto... args) {
    _PyErr_Clear(tstate);
    PyObject *funcstr = _PyObject_FunctionStr(func);
    if (funcstr) {
        _PyErr_Format(tstate, PyExc_TypeError, error, funcstr, args...);
        Py_DECREF(funcstr);
    }
}

PyObject *handle_CALL_FUNCTION_EX(PyObject *func, PyObject *args, PyObject *kwargs) {
    // According to Python/compile.c, kwargs is always a dict instance (or null).
    assert(!kwargs || PyDict_CheckExact(kwargs));
    PyObject *ret = nullptr;
    if (PyTuple_CheckExact(args)) {
        ret = PyObject_Call(func, args, kwargs);
    } else {
        auto type = Py_TYPE(args);
        if (!type->tp_iter && (PyDict_Check(args) || !type->tp_as_sequence || !type->tp_as_sequence->sq_item)) {
            auto tstate = getThreadState();
            formatFunctionCallError(tstate, func,
                    "%U argument after * must be an iterable, not %.200s", Py_TYPE(args)->tp_name);
            gotoErrorHandler(tstate);
        }
        auto t = PySequence_Tuple(args);
        gotoErrorHandlerIf(!t);
        ret = PyObject_Call(func, t, kwargs);
        Py_DECREF(t);
    }
    /* Note: tracefunc support here */
    gotoErrorHandlerIf(!ret);
    return ret;
}

PyObject *handle_MAKE_FUNCTION(PyObject *codeobj, PyFrameObject *f, PyObject *qualname,
        PyObject **extra, int flag) {
    auto func = reinterpret_cast<PyFunctionObject *>(PyFunction_NewWithQualName(codeobj, f->f_globals, qualname));
    gotoErrorHandlerIf(!func);
    if (flag & 0x08) {
        assert(PyTuple_CheckExact(extra[-1]));
        func->func_closure = *--extra;
    }
    if (flag & 0x04) {
        assert(PyTuple_CheckExact(extra[-1]));
        func->func_annotations = *--extra;
    }
    if (flag & 0x02) {
        assert(PyDict_CheckExact(extra[-1]));
        func->func_kwdefaults = *--extra;
    }
    if (flag & 0x01) {
        assert(PyTuple_CheckExact(extra[-1]));
        func->func_defaults = *--extra;
    }
    return reinterpret_cast<PyObject *>(func);
}

void handle_FOR_ITER(PyObject *iter) {
    auto tstate = getThreadState();
    if (_PyErr_Occurred(tstate)) {
        gotoErrorHandlerIf(!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration), tstate);
        /* Note: tracefunc support here */
        _PyErr_Clear(tstate);
    }
    Py_DECREF(iter);
}

PyObject *handle_BUILD_STRING(PyObject **arr, Py_ssize_t num) {
    auto str = PyUnicode_New(0, 0);
    gotoErrorHandlerIf(!str);
    str = _PyUnicode_JoinArray(str, arr, num);
    gotoErrorHandlerIf(!str);
    while (--num >= 0) {
        Py_DECREF(arr[num]);
    }
    return str;
}

PyObject *handle_BUILD_TUPLE(PyObject **arr, Py_ssize_t num) {
    auto tup = PyTuple_New(num);
    gotoErrorHandlerIf(!tup);
    while (--num >= 0) {
        PyTuple_SET_ITEM(tup, num, arr[num]);
    }
    return tup;
}

PyObject *handle_BUILD_LIST(PyObject **arr, Py_ssize_t num) {
    auto list = PyList_New(num);
    gotoErrorHandlerIf(!list);
    while (--num >= 0) {
        PyList_SET_ITEM(list, num, arr[num]);
    }
    return list;
}

PyObject *handle_BUILD_SET(PyObject **arr, Py_ssize_t num) {
    auto *set = PySet_New(nullptr);
    gotoErrorHandlerIf(!set);
    for (auto i = 0; i < num; i++) {
        if (PySet_Add(set, arr[i])) {
            Py_DECREF(set);
            gotoErrorHandler();
        }
    }
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[i]);
    }
    return set;
}

PyObject *handle_BUILD_MAP(PyObject **arr, Py_ssize_t num) {
    auto map = _PyDict_NewPresized(num);
    gotoErrorHandlerIf(!map);
    for (auto i = 0; i < num; i++) {
        if (PyDict_SetItem(map, arr[2 * i], arr[2 * i + 1])) {
            Py_DECREF(map);
            gotoErrorHandler();
        }
    }
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[2 * i]);
        Py_DECREF(arr[2 * i + 1]);
    }
    return map;
}

PyObject *handle_BUILD_CONST_KEY_MAP(PyObject **arr, Py_ssize_t num) {
    auto keys = arr[num];
    formatErrorAndGotoHandlerIf(!PyTuple_CheckExact(keys) || PyTuple_GET_SIZE(keys) != num,
            PyExc_SystemError, "bad BUILD_CONST_KEY_MAP keys argument");
    auto map = _PyDict_NewPresized(num);
    gotoErrorHandlerIf(!map);
    for (auto i = 0; i < num; i++) {
        if (PyDict_SetItem(map, PyTuple_GET_ITEM(keys, i), arr[i])) {
            Py_DECREF(map);
            gotoErrorHandler();
        }
    }
    Py_DECREF(keys);
    for (auto i = 0; i < num; i++) {
        Py_DECREF(arr[i]);
    }
    return map;
}

void handle_LIST_APPEND(PyObject *list, PyObject *value) {
    gotoErrorHandlerIf(PyList_Append(list, value));
}

void handle_SET_ADD(PyObject *set, PyObject *value) {
    gotoErrorHandlerIf(PySet_Add(set, value));
}

void handle_MAP_ADD(PyObject *map, PyObject *key, PyObject *value) {
    gotoErrorHandlerIf(PyDict_SetItem(map, key, value));
}

void handle_LIST_EXTEND(PyObject *list, PyObject *iterable) {
    PyObject *none_val = _PyList_Extend((PyListObject *) list, iterable);
    if (none_val) [[likely]] {
        Py_DECREF(none_val);
        return;
    }
    auto tstate = getThreadState();
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError)
            && !Py_TYPE(iterable)->tp_iter && !PySequence_Check(iterable)) {
        _PyErr_Clear(tstate);
        _PyErr_Format(tstate, PyExc_TypeError,
                "Value after * must be an iterable, not %.200s",
                Py_TYPE(iterable)->tp_name);
    }
    gotoErrorHandler(tstate);
}

void handle_SET_UPDATE(PyObject *set, PyObject *iterable) {
    gotoErrorHandlerIf(_PySet_Update(set, iterable) < 0);
}

void handle_DICT_UPDATE(PyObject *dict, PyObject *update) {
    if (PyDict_Update(dict, update) >= 0) [[likely]] {
        return;
    }
    auto tstate = getThreadState();
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Format(tstate, PyExc_TypeError, "'%.200s' object is not a mapping", Py_TYPE(update)->tp_name);
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_LIST_TO_TUPLE(PyObject *list) {
    auto tuple = PyList_AsTuple(list);
    gotoErrorHandlerIf(!tuple);
    return tuple;
}

void handle_DICT_MERGE(PyObject *func, PyObject *dict, PyObject *update) {
    if (_PyDict_MergeEx(dict, update, 2) == 0) [[likely]] {
        return;
    }
    auto tstate = getThreadState();
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        formatFunctionCallError(tstate, func,
                "%U argument after ** must be a mapping, not %.200s", Py_TYPE(update)->tp_name);
    } else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        auto val = tstate->curexc_value;
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            PyObject *key = PyTuple_GET_ITEM(val, 0);
            Py_INCREF(key);
            formatFunctionCallError(tstate, func, "%U got multiple values for keyword argument '%S'", key);
            Py_DECREF(key);
        }
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_FORMAT_VALUE(PyObject *value, PyObject *fmt_spec, int which_conversion) {
    switch (which_conversion) {
    case FVC_NONE:
        Py_INCREF(value);
        break;
    case FVC_STR:
        gotoErrorHandlerIf(!(value = PyObject_Str(value)));
        break;
    case FVC_REPR:
        gotoErrorHandlerIf(!(value = PyObject_Repr(value)));
        break;
    case FVC_ASCII:
        gotoErrorHandlerIf(!(value = PyObject_ASCII(value)));
        break;
    default: {
        formatErrorAndGotoHandler(PyExc_SystemError, "unexpected conversion flag %d", which_conversion);
    }
    }

    if (!fmt_spec && PyUnicode_CheckExact(value)) {
        return value;
    }
    auto fmt_value = PyObject_Format(value, fmt_spec);
    Py_DECREF(value);
    gotoErrorHandlerIf(!fmt_value);
    return fmt_value;
}

PyObject *handle_BUILD_SLICE(PyObject *start, PyObject *stop, PyObject *step) {
    auto slice = PySlice_New(start, stop, step);
    gotoErrorHandlerIf(!slice);
    return slice;
}

void handle_SETUP_ANNOTATIONS(PyFrameObject *f) {
    _Py_IDENTIFIER(__annotations__);
    formatErrorAndGotoHandlerIf(!f->f_locals, PyExc_SystemError, "no locals found when setting up annotations");
    /* check if __annotations__ in locals()... */
    if (PyDict_CheckExact(f->f_locals)) {
        auto ann_dict = _PyDict_GetItemIdWithError(f->f_locals, &PyId___annotations__);
        if (!ann_dict) {
            gotoErrorHandlerIfErrorOccurred();
            /* ...if not, create a new one */
            ann_dict = PyDict_New();
            gotoErrorHandlerIf(!ann_dict);
            auto err = _PyDict_SetItemId(f->f_locals, &PyId___annotations__, ann_dict);
            Py_DECREF(ann_dict);
            gotoErrorHandlerIf(err);
        }
    } else {
        /* do the same if locals() is not a dict */
        PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
        gotoErrorHandlerIf(!ann_str);
        auto ann_dict = PyObject_GetItem(f->f_locals, ann_str);
        if (!ann_dict) {
            auto tstate = getThreadState();
            gotoErrorHandlerIf(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError));
            _PyErr_Clear(tstate);
            ann_dict = PyDict_New();
            gotoErrorHandlerIf(!ann_dict);
            auto err = PyObject_SetItem(f->f_locals, ann_str, ann_dict);
            Py_DECREF(ann_dict);
            gotoErrorHandlerIf(err);
        } else {
            Py_DECREF(ann_dict);
        }
    }
}

void handle_PRINT_EXPR(PyObject *value) {
    _Py_IDENTIFIER(displayhook);
    PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
    formatErrorAndGotoHandlerIf(!hook, PyExc_RuntimeError, "lost sys.displayhook");
    auto res = PyObject_CallOneArg(hook, value);
    gotoErrorHandlerIf(!res);
    Py_DECREF(res);
}

void handle_UNPACK_SEQUENCE(PyObject *seq, Py_ssize_t num, PyObject **dest) {
    if (PyTuple_CheckExact(seq) && PyTuple_GET_SIZE(seq) == num) {
        auto items = (reinterpret_cast<PyTupleObject *>(seq))->ob_item;
        while (num--) {
            auto item = items[num];
            Py_INCREF(item);
            *dest++ = item;
        }
    } else if (PyList_CheckExact(seq) && PyList_GET_SIZE(seq) == num) {
        auto items = (reinterpret_cast<PyListObject *>(seq))->ob_item;
        while (num--) {
            auto item = items[num];
            Py_INCREF(item);
            *dest++ = item;
        }
    } else {
        handle_UNPACK_EX(seq, num, -1, dest);
    }
}

void handle_UNPACK_EX(PyObject *seq, Py_ssize_t before_star, Py_ssize_t after_star, PyObject **dest) {
    assert(seq);
    auto old_stack_value = *dest;
    auto ptr_end = dest + (before_star + 1 + after_star);
    auto ptr = ptr_end;
    auto tstate = getThreadState();
    auto iter = PyObject_GetIter(seq);
    const auto &clear_and_exit = [&]() {
        Py_DECREF(iter);
        while (ptr != ptr_end) {
            Py_DECREF(*ptr++);
        }
        // Note: The value on the stack may have been overwritten, we should restore it.
        // Otherwise, gotoErrorHandler will Py_DECREF the wrong value.
        *dest = old_stack_value;
        gotoErrorHandler(tstate);
    };

    if (!iter) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) && !Py_TYPE(seq)->tp_iter && !PySequence_Check(seq)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                    "cannot unpack non-iterable %.200s object",
                    Py_TYPE(seq)->tp_name);
        }
        gotoErrorHandler();
    }

    for (int i = 0; i < before_star; i++) {
        if (auto next = PyIter_Next(iter)) {
            *--ptr = next;
            continue;
        }
        /* Iterator done, via error or exhaustion. */
        formatErrorIfNotOccurred(tstate, PyExc_ValueError,
                "not enough values to unpack (expected %s%d, got %d)",
                after_star == -1 ? "" : "at least ",
                after_star == -1 ? before_star : before_star + after_star,
                i);
        clear_and_exit();
    }

    if (after_star == -1) {
        /* We better have exhausted the iterator now. */
        auto w = PyIter_Next(iter);
        if (w) {
            Py_DECREF(w);
            _PyErr_Format(tstate, PyExc_ValueError, "too many values to unpack (expected %d)", before_star);
            clear_and_exit();
        }
        if (_PyErr_Occurred(tstate)) {
            clear_and_exit();
        }
        Py_DECREF(iter);
        return;
    }

    auto l = PySequence_List(iter);
    if (!l) {
        clear_and_exit();
    }
    *--ptr = l;

    auto list_size = PyList_GET_SIZE(l);
    if (list_size < after_star) {
        _PyErr_Format(tstate, PyExc_ValueError,
                "not enough values to unpack (expected at least %d, got %zd)",
                before_star + after_star, before_star + list_size);
        clear_and_exit();
    }

    /* Pop the "after-variable" args off the list. */
    for (int j = after_star; j > 0; j--) {
        *--ptr = PyList_GET_ITEM(l, list_size - j);
    }
    /* Resize the list. */
    Py_SET_SIZE(l, list_size - after_star);
    Py_DECREF(iter);
}

PyObject *hanlde_GET_LEN(PyObject *value) {
    auto len_i = PyObject_Length(value);
    gotoErrorHandlerIf(len_i < 0);
    auto len_o = PyLong_FromSsize_t(len_i);
    gotoErrorHandlerIf(!len_o);
    return len_o;
}

void hanlde_MATCH_KEYS(PyObject **inputs_outputs) {
    auto keys = inputs_outputs[-1];
    auto map = inputs_outputs[-2];
    auto nkeys = PyTuple_GET_SIZE(keys);
    if (!nkeys) {
        auto empty_tuple = PyTuple_New(0);
        gotoErrorHandlerIf(!empty_tuple);
        inputs_outputs[0] = empty_tuple;
        inputs_outputs[1] = Py_NewRef(Py_True);
        return;
    }

    _Py_IDENTIFIER(get);
    auto get = _PyObject_GetAttrId(map, &PyId_get);
    gotoErrorHandlerIf(!get);
    auto seen = PySet_New(nullptr);
    if (!seen) {
        Py_DECREF(get);
        gotoErrorHandler();
    }
    auto values = PyList_New(0);
    if (!seen) {
        Py_DECREF(get);
        Py_DECREF(seen);
        gotoErrorHandler();
    }

    const auto &do_match = [=]() -> PyObject * {
        static PyObject dummy = {_PyObject_EXTRA_INIT 1, &PyBaseObject_Type};
        for (auto i : IntRange(nkeys)) {
            auto key = PyTuple_GET_ITEM(keys, i);
            if (PySet_Contains(seen, key) || PySet_Add(seen, key)) {
                formatErrorIfNotOccurred(getThreadState(),
                        PyExc_ValueError, "mapping pattern checks duplicate key (%R)", key);
                return nullptr;
            }
            auto value = PyObject_CallFunctionObjArgs(get, key, &dummy, nullptr);
            if (!value) {
                return nullptr;
            }
            if (value == &dummy) {
                Py_DECREF(value);
                return Py_NewRef(Py_None);
            }
            Py_DECREF(value);
            if (PyList_Append(values, value)) {
                return nullptr;
            }
        }

        auto tuple_values = PyList_AsTuple(values);
        return tuple_values;
    };

    auto result = do_match();
    Py_DECREF(get);
    Py_DECREF(seen);
    Py_DECREF(values);
    gotoErrorHandlerIf(!result);
    inputs_outputs[0] = result;
    inputs_outputs[1] = Py_NewRef(result != Py_None ? Py_True : Py_False);
}

void hanlde_MATCH_CLASS(Py_ssize_t nargs, PyObject *kwargs, PyObject **inputs_outputs) {
    auto subject = inputs_outputs[0];
    auto type = inputs_outputs[1];
    auto type_ = reinterpret_cast<PyTypeObject *>(type);

    formatErrorAndGotoHandlerIf(!PyType_Check(type), PyExc_TypeError, "called match pattern must be a type");

    PyObject *seen = nullptr;
    PyObject *attrs = nullptr;
    PyObject *match_args = nullptr;

    const auto &match_class_attr = [&](PyObject *name) {
        if (PySet_Contains(seen, name) || PySet_Add(seen, name)) {
            formatErrorIfNotOccurred(getThreadState(), PyExc_TypeError,
                    "%s() got multiple sub-patterns for attribute %R", type_->tp_name, name);
            return false;
        }
        auto attr = PyObject_GetAttr(subject, name);
        if (attr) {
            auto err = PyList_Append(attrs, attr);
            Py_DECREF(attr);
            return !err;
        }
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
        }
        return false;
    };

    const auto &do_match = [=, &seen, &attrs, &match_args]() -> PyObject * {
        if (PyObject_IsInstance(subject, type) <= 0) {
            return nullptr;
        }
        seen = PySet_New(nullptr);
        if (!seen) {
            return nullptr;
        }
        attrs = PyList_New(0);
        if (!attrs) {
            return nullptr;
        }

        // First, the positional subpatterns:
        if (nargs) {
            bool match_self = false;
            match_args = PyObject_GetAttrString(type, "__match_args__");
            if (match_args) {
                if (!PyTuple_CheckExact(match_args)) {
                    PyErr_Format(PyExc_TypeError, "%s.__match_args__ must be a tuple (got %s)",
                            type_->tp_name,
                            Py_TYPE(match_args)->tp_name);
                    return nullptr;
                }
            } else if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                // _Py_TPFLAGS_MATCH_SELF is only acknowledged if the type does not
                // define __match_args__. This is natural behavior for subclasses:
                // it's as if __match_args__ is some "magic" value that is lost as
                // soon as they redefine it.
                match_args = PyTuple_New(0);
                if (!match_args) {
                    return nullptr;
                }
                match_self = PyType_HasFeature(type_, _Py_TPFLAGS_MATCH_SELF);
            } else {
                return nullptr;
            }
            assert(PyTuple_CheckExact(match_args));
            auto allowed = match_self ? 1 : PyTuple_GET_SIZE(match_args);
            if (allowed < nargs) {
                PyErr_Format(PyExc_TypeError,
                        "%s() accepts %d positional sub-pattern%s (%d given)",
                        type_->tp_name,
                        allowed, (allowed == 1) ? "" : "s", nargs);
                return nullptr;
            }
            if (match_self) {
                // Easy. Copy the subject itself, and move on to kwargs.
                if (PyList_Append(attrs, subject)) {
                    return nullptr;
                }
            } else {
                for (auto i : IntRange(nargs)) {
                    PyObject *name = PyTuple_GET_ITEM(match_args, i);
                    if (!PyUnicode_CheckExact(name)) {
                        PyErr_Format(PyExc_TypeError,
                                "__match_args__ elements must be strings (got %s)", Py_TYPE(name)->tp_name);
                        return nullptr;
                    }
                    if (!match_class_attr(name)) {
                        return nullptr;
                    }
                }
            }
            Py_CLEAR(match_args);
        }

        // Finally, the keyword subpatterns:
        for (auto i : IntRange(PyTuple_GET_SIZE(kwargs))) {
            PyObject *name = PyTuple_GET_ITEM(kwargs, i);
            if (!match_class_attr(name)) {
                return nullptr;
            }
        }

        auto tuple_values = PyList_AsTuple(attrs);
        return tuple_values;
    };

    auto result = do_match();
    if (!result) {
        Py_XDECREF(seen);
        Py_XDECREF(attrs);
        Py_XDECREF(match_args);
        gotoErrorHandlerIfErrorOccurred();
        inputs_outputs[1] = Py_NewRef(Py_False);
        Py_DECREF(type);
    } else {
        Py_DECREF(seen);
        Py_DECREF(attrs);
        inputs_outputs[0] = result;
        inputs_outputs[1] = Py_NewRef(Py_True);
        Py_DECREF(subject);
        Py_DECREF(type);
    }
}

PyObject *handle_COPY_DICT_WITHOUT_KEYS(PyObject *subject, PyObject *keys) {
    PyObject *rest = PyDict_New();
    gotoErrorHandlerIf(!rest);
    if (PyDict_Update(rest, subject)) {
        Py_DECREF(rest);
        gotoErrorHandler();
    }
    assert(PyTuple_CheckExact(keys));
    for (auto i : IntRange(PyTuple_GET_SIZE(keys))) {
        if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
            Py_DECREF(rest);
            gotoErrorHandler();
        }
    }
    return rest;
}


PyObject *handle_LOAD_BUILD_CLASS(PyFrameObject *f) {
    _Py_IDENTIFIER(__build_class__);
    PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
    gotoErrorHandlerIf(!build_class_str);

    auto builtins = f->f_builtins;
    static const char error_message[]{"__build_class__ not found"};
    if (PyDict_CheckExact(builtins)) {
        auto hash = reinterpret_cast<PyASCIIObject *>(build_class_str)->hash;
        auto bc = _PyDict_GetItem_KnownHash(builtins, build_class_str, hash);
        if (bc) {
            Py_INCREF(bc);
            return bc;
        }
        auto tstate = getThreadState();
        formatErrorIfNotOccurred(tstate, PyExc_NameError, error_message);
        gotoErrorHandler(tstate);
    } else {
        auto bc = PyObject_GetItem(builtins, build_class_str);
        if (bc) {
            return bc;
        }
        auto tstate = getThreadState();
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            _PyErr_SetString(tstate, PyExc_NameError, error_message);
        }
        gotoErrorHandler(tstate);
    }
}

PyObject *handle_IMPORT_NAME(PyFrameObject *f, PyObject *name, PyObject *fromlist, PyObject *level) {
    auto tstate = getThreadState();
    auto res = import_name(tstate, f, name, fromlist, level);
    gotoErrorHandlerIf(!res, tstate);
    return res;
}

PyObject *handle_IMPORT_FROM(PyObject *from, PyObject *name) {
    auto tstate = getThreadState();
    auto res = import_from(tstate, from, name);
    gotoErrorHandlerIf(!res, tstate);
    return res;
}

void handle_IMPORT_STAR(PyFrameObject *f, PyObject *from) {
    gotoErrorHandlerIf(PyFrame_FastToLocalsWithError(f) < 0);
    auto tstate = getThreadState();
    formatErrorAndGotoHandlerIf(!f->f_locals, tstate, PyExc_SystemError, "no locals found during 'import *'");
    auto err = import_all_from(tstate, f->f_locals, from);
    PyFrame_LocalsToFast(f, 0);
    gotoErrorHandlerIf(err, tstate);
}

void handle_POP_EXCEPT(PyFrameObject *f, PyObject **exc_triple) {
    auto tstate = getThreadState();
    formatErrorAndGotoHandlerIf(PyFrame_BlockPop(f)->b_type != EXCEPT_HANDLER,
            tstate, PyExc_SystemError, "popped block is not an except handler");
    restoreExcInfo(tstate, exc_triple);
}

bool handle_JUMP_IF_NOT_EXC_MATCH(PyObject *left, PyObject *right) {
    static char CANNOT_CATCH_MSG[]{"catching classes that do not inherit from BaseException is not allowed"};
    if (PyTuple_Check(right)) {
        for (auto i = PyTuple_GET_SIZE(right); i--;) {
            auto exc = PyTuple_GET_ITEM(right, i);
            formatErrorAndGotoHandlerIf(!PyExceptionClass_Check(exc), PyExc_TypeError, CANNOT_CATCH_MSG);
        }
    } else {
        formatErrorAndGotoHandlerIf(!PyExceptionClass_Check(right), PyExc_TypeError, CANNOT_CATCH_MSG);
    }
    auto res = PyErr_GivenExceptionMatches(left, right);
    gotoErrorHandlerIf(res < 0);
    return res > 0;
}

[[noreturn]] void handle_RERAISE(bool restore_lasti, int stack_height) {
    auto tstate = getThreadState();
    auto f = tstate->frame;
    assert(f->f_iblock > 0);
    if (restore_lasti) {
        f->f_lasti = f->f_blockstack[f->f_iblock - 1].b_handler;
    }
    auto sp = &f->f_valuestack[stack_height -= 3];
    _PyErr_Restore(tstate, sp[2], sp[1], sp[0]);
    unwindFrame(tstate, stack_height);
}

void handle_SETUP_WITH(PyFrameObject *f, int handler, int stack_height) {
    _Py_IDENTIFIER(__enter__);
    _Py_IDENTIFIER(__exit__);

    auto *sp = f->f_valuestack + stack_height;
    auto mgr = sp[-1];
    auto enter = _PyObject_LookupSpecial(mgr, &PyId___enter__);
    if (!enter) {
        auto tstate = getThreadState();
        formatErrorIfNotOccurred(tstate, PyExc_AttributeError, &PyId___enter__);
        gotoErrorHandler(tstate);
    }

    auto exit = _PyObject_LookupSpecial(mgr, &PyId___exit__);
    if (!exit) {
        auto tstate = getThreadState();
        formatErrorIfNotOccurred(tstate, PyExc_AttributeError, &PyId___exit__);
        Py_DECREF(enter);
        gotoErrorHandler(tstate);
    }
    sp[-1] = exit;
    Py_DECREF(mgr);
    auto res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    gotoErrorHandlerIf(!res);
    PyFrame_BlockSetup(f, SETUP_FINALLY, handler, stack_height);
    sp[0] = res;
}

PyObject *handle_WITH_EXCEPT_START(PyObject **stack_top) {
    PyObject *args[4] = {nullptr, stack_top[-1], stack_top[-2], stack_top[-3]};
    auto res = PyObject_Vectorcall(stack_top[-7], &args[1], 3 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
    gotoErrorHandlerIf(!res);
    return res;
}

[[noreturn]] void handle_RAISE_VARARGS(int argc, int stack_height) {
    auto tstate = getThreadState();
    auto sp = &tstate->frame->f_valuestack[stack_height - argc];
    PyObject *exc = nullptr;
    PyObject *cause = nullptr;
    if (argc) {
        exc = sp[0];
        Py_INCREF(exc);
        if (argc == 2) {
            cause = sp[1];
            Py_INCREF(cause);
        }
    }
    if (do_raise(tstate, exc, cause)) {
        unwindFrame(tstate, stack_height);
    }
    gotoErrorHandler(tstate);
}

PyObject *handle_YIELD_VALUE(PyObject *val) {
    PyObject *w = _PyAsyncGenValueWrapperNew(val);
    gotoErrorHandlerIf(!w);
    Py_DECREF(val);
    return w;
}

PyObject *handle_YIELD_FROM(PyObject **inputs_outputs) {
    auto receiver = inputs_outputs[0];
    auto arg = inputs_outputs[1];
    PyObject *value;
    auto gen_status = PyIter_Send(receiver, arg, &value);
    /* Note: tracefunc support here */
    gotoErrorHandlerIf(gen_status == PYGEN_ERROR);
    Py_DECREF(arg);
    if (gen_status == PYGEN_RETURN) {
        inputs_outputs[0] = value;
        Py_DECREF(receiver);
        return nullptr;
    } else {
        return value;
    }
}

PyObject *handle_GET_YIELD_FROM_ITER(PyObject *iterable, bool is_coroutine) {
    if (PyCoro_CheckExact(iterable)) {
        formatErrorAndGotoHandlerIf(!is_coroutine, PyExc_TypeError,
                "cannot 'yield from' a coroutine object in a non-coroutine generator");
    } else if (!PyGen_CheckExact(iterable)) {
        auto iter = PyObject_GetIter(iterable);
        gotoErrorHandlerIf(!iter);
        return iter;
    }
    Py_INCREF(iterable);
    return iterable;
}

PyObject *handle_GET_AWAITABLE(PyObject *iterable, int prev_prev_op, int prev_op) {
    auto iter = _PyCoro_GetAwaitableIter(iterable);
    if (!iter) {
        auto tstate = getThreadState();
        format_awaitable_error(tstate, Py_TYPE(iterable), prev_prev_op, prev_op);
        gotoErrorHandler(tstate);
    }

    if (PyCoro_CheckExact(iter)) {
        PyObject *yf = _PyGen_yf(reinterpret_cast<PyGenObject *>(iter));
        if (yf) {
            Py_DECREF(yf);
            Py_DECREF(iter);
            formatErrorAndGotoHandler(PyExc_RuntimeError, "coroutine is being awaited already");
        }
    }
    return iter;
}

PyObject *handle_GET_AITER(PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);
    formatErrorAndGotoHandlerIf(!type->tp_as_async || !type->tp_as_async->am_aiter, PyExc_TypeError,
            "'async for' requires an object with __aiter__ method, got %.100s",
            type->tp_name);

    auto iter = type->tp_as_async->am_aiter(obj);
    gotoErrorHandlerIf(!iter);
    auto iter_type = Py_TYPE(iter);

    if (!iter_type->tp_as_async || !iter_type->tp_as_async->am_anext) {
        auto tstate = getThreadState();
        _PyErr_Format(tstate, PyExc_TypeError,
                "'async for' received an object from __aiter__ that does not implement __anext__: %.100s",
                iter_type->tp_name);
        Py_DECREF(iter);
        gotoErrorHandler(tstate);
    }
    return iter;
}

PyObject *handle_GET_ANEXT(PyObject *aiter) {
    PyTypeObject *type = Py_TYPE(aiter);
    if (PyAsyncGen_CheckExact(aiter)) {
        auto awaitable = type->tp_as_async->am_anext(aiter);
        gotoErrorHandlerIf(!awaitable);
        return awaitable;
    } else {
        formatErrorAndGotoHandlerIf(!type->tp_as_async || !type->tp_as_async->am_anext, PyExc_TypeError,
                "'async for' requires an iterator with __anext__ method, got %.100s",
                type->tp_name);
        auto next_iter = type->tp_as_async->am_anext(aiter);
        gotoErrorHandlerIf(!next_iter);

        auto awaitable = _PyCoro_GetAwaitableIter(next_iter);
        if (!awaitable) {
            _PyErr_FormatFromCause(PyExc_TypeError, "'async for' received an invalid object from __anext__: %.100s",
                    Py_TYPE(next_iter)->tp_name);
            Py_DECREF(next_iter);
            gotoErrorHandler();
        }
        Py_DECREF(next_iter);
        return awaitable;
    }
}

void handle_END_ASYNC_FOR(int stack_height) {
    auto tstate = getThreadState();
    auto f = tstate->frame;
    if (PyErr_GivenExceptionMatches(f->f_valuestack[stack_height - 1], PyExc_StopAsyncIteration)) {
        auto b = PyFrame_BlockPop(f);
        assert(b->b_type == EXCEPT_HANDLER);
        clearStack(f, stack_height, b->b_level + 3);
        restoreExcInfo(tstate, &f->f_valuestack[stack_height -= 3]);
        Py_DECREF(f->f_valuestack[--stack_height]);
    } else {
        auto sp = &f->f_valuestack[stack_height -= 3];
        _PyErr_Restore(tstate, sp[2], sp[1], sp[0]);
        unwindFrame(tstate, stack_height);
    }
}

void handle_BEFORE_ASYNC_WITH(PyObject **sp) {
    _Py_IDENTIFIER(__aenter__);
    _Py_IDENTIFIER(__aexit__);

    auto mgr = *--sp;
    auto enter = _PyObject_LookupSpecial(mgr, &PyId___aenter__);
    if (!enter) {
        auto tstate = getThreadState();
        formatErrorIfNotOccurred(tstate, PyExc_AttributeError, &PyId___aenter__);
        gotoErrorHandler(tstate);
    }

    auto exit = _PyObject_LookupSpecial(mgr, &PyId___aexit__);
    if (!exit) {
        auto tstate = getThreadState();
        formatErrorIfNotOccurred(tstate, PyExc_AttributeError, &PyId___aexit__);
        Py_DECREF(enter);
        gotoErrorHandler(tstate);
    }
    *sp++ = exit;
    Py_DECREF(mgr);
    auto res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    gotoErrorHandlerIf(!res);
    *sp++ = res;
}
