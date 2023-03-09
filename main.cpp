#include <Python.h>
#undef HAVE_STD_ATOMIC
#include <frameobject.h>
#include <internal/pycore_interp.h>
#include <internal/pycore_ceval.h>
#include <internal/pycore_pyerrors.h>

#include "compilation_unit.h"
#include "ported/interface.h"
#include "types.h"

static Translator *translator;
PyObject compilation_error;
int jit_threshold = 0x4000;

void notifyCodeLoaded(void *bin_addr, PyObject *py_code) {}

static TranslatedResult *compilePythonCode(PyCode py_code, PyObject *debug_args) {
    // Note: Make sure there are no errors raised before compiling.
    assert(!PyErr_Occurred());

    BinCodeCache bin_code_cache{py_code};
    auto result = bin_code_cache.load();
    if (!result) {
        if (!translator) {
            alignas(Translator) static char buffuer[sizeof(Translator)];
            translator = new(buffuer) Translator();
            if (!translator->initialize()) {
                return nullptr;
            }
        }
        CompilationUnit cu{*translator, py_code};
        if (cu.translate(debug_args)) {
            result = bin_code_cache.store(cu);
        }
    }
    if (result) {
        if (_PyCode_SetExtra(py_code, code_extra_index, result) == 0) {
            notifyCodeLoaded(result->entry_address(), py_code);
            return result;
        }
    }
    return nullptr;
}

template <bool new_eval, typename T>
static PyObject *evalFrame(PyThreadState *tstate, PyFrameObject *f, T throwflag_or_vpc) {
    if constexpr (new_eval) {
        if (throwflag_or_vpc || tstate->cframe->use_tracing) {
            return Ported_PyEval_EvalFrameDefault(tstate, f, throwflag_or_vpc);
        }
    }

    TranslatedResult *translated_result;
    if (hasTranslatedResult(f->f_code)) {
        translated_result = &getTranslatedResult(f->f_code);
    } else {
        if constexpr (new_eval) {
            if (f->f_code->co_opcache_flag <= jit_threshold) {
                return Ported_PyEval_EvalFrameDefault(tstate, f, throwflag_or_vpc);
            }
            translated_result = compilePythonCode(f->f_code, nullptr);
            if (!translated_result) {
                return nullptr;
            }
        } else {
            translated_result = compilePythonCode(f->f_code, nullptr);
            if (!translated_result) {
                f->f_code->co_opcache_flag = std::numeric_limits<decltype(f->f_code->co_opcache_flag)>::min();
                return &compilation_error;
            }
        }
    }

    ExtendedCFrame cframe{
            {tstate->cframe->use_tracing, tstate->cframe},
            translated_result
    };
    if constexpr (new_eval) {
        cframe.handler = f->f_lasti < 0 ? 0 : translated_result->calcPC(f->f_lasti + 1);

        if (_Py_EnterRecursiveCall(tstate, "")) {
            return nullptr;
        }
        tstate->frame = f;
        f->f_stackdepth = -1;
        /* Note: tracefunc support here */
        f->f_state = FRAME_EXECUTING;
    } else {
        cframe.handler = translated_result->calcPC(throwflag_or_vpc);
    }

    tstate->cframe = &cframe;

    PyObject *ret_val = nullptr;
    void *eval_breaker = &tstate->interp->ceval.eval_breaker;
    if (setjmp(cframe.frame_jmp_buf) >= 0) {
        assert(!_PyErr_Occurred(tstate));
        ret_val = (*translated_result)(RuntimeSymbols::address_array.data(), f, &cframe, eval_breaker);
    }
    assert(!ret_val ^ !_PyErr_Occurred(tstate));
    assert(f->f_state == FRAME_SUSPENDED || !f->f_stackdepth);

    tstate->cframe = cframe.previous;
    tstate->cframe->use_tracing = cframe.use_tracing;
    if constexpr (new_eval) {
        tstate->frame = f->f_back;
        _Py_LeaveRecursiveCall(tstate);
    }
    return ret_val;
}

PyObject *tackOverFrame(PyThreadState *tstate, PyFrameObject *f, int vpc) {
    return evalFrame<false, IntVPC>(tstate, f, vpc);
}

static PyObject *compile(PyObject *, PyObject *func) {
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "not a function object");
        return nullptr;
    }
    if (!compilePythonCode(reinterpret_cast<PyFunctionObject *>(func)->func_code, nullptr)) {
        return nullptr;
    }
    return Py_NewRef(func);
}

#ifdef DUMP_DEBUG_FILES
static PyObject *debugCompile(PyObject *, PyObject *debug_args) {
    if (!compilePythonCode(PyTuple_GET_ITEM(debug_args, 0), debug_args)) {
        return nullptr;
    }
    return Py_NewRef(Py_None);
}
#endif


PyObject *setup() {
    if (compyler_module) {
        return Py_NewRef(compyler_module);
    }

    if (auto env_value = getenv("COMPYLER_THRESHOLD_RATIO")) {
        char *end;
        auto ratio = strtod(env_value, &end);
        if (end != env_value && *end == '\0') {
            auto new_threshold = jit_threshold * ratio;
            jit_threshold = new_threshold > INT_MAX ? INT_MAX : static_cast<int>(new_threshold);
        }
    }
    if (auto env_value = getenv("COMPYLER_CACHE_ROOT")) {
        BinCodeCache::setCacheRoot(env_value);
    }
#ifdef ABLATION_BUILD
    if (auto env_value = getenv("COMPYLER_SOE")) {
        with_SOE = strcmp(env_value, "0");
    }
    if (auto env_value = getenv("COMPYLER_ICE")) {
        with_ICE = strcmp(env_value, "0");
    }
    if (auto env_value = getenv("COMPYLER_IRO")) {
        with_IRO = !strcmp(env_value, "1");
    }
#endif

    static PyMethodDef meth_def[]{
            {"compile", compile, METH_O},
#ifdef DUMP_DEBUG_FILES
            {"_debug_compile", debugCompile, METH_O},
#endif
            {}
    };
    static PyModuleDef mod_def{
            .m_base=PyModuleDef_HEAD_INIT,
            .m_name="comPyler",
            .m_size=-1,
            .m_methods=meth_def,
            .m_free=[](void *) {
                _PyInterpreterState_SetEvalFrameFunc(PyInterpreterState_Get(), _PyEval_EvalFrameDefault);
                if (translator) {
                    translator->~Translator();
                }
                ExeMemBlock::clear();
            }
    };

    if ((code_extra_index = _PyEval_RequestCodeExtraIndex(TranslatedResult::destroy)) < 0) {
        return nullptr;
    }

    if (!(compyler_module = PyModule_Create(&mod_def))) {
        return nullptr;
    }
    _PyInterpreterState_SetEvalFrameFunc(PyInterpreterState_Get(), evalFrame<true, int>);
    return compyler_module;
}

PyMODINIT_FUNC PyInit_usercustomize() { return setup(); }
PyMODINIT_FUNC PyInit_compyler() { return setup(); }
