#include "Python.h"
#include "internal/pycore_object.h"
#include "frameobject.h"
#include "opcode.h"


PyObject *
_PyGen_yf(PyGenObject *gen)
{
    PyObject *yf = NULL;
    PyFrameObject *f = gen->gi_frame;

    if (f) {
        PyObject *bytecode = f->f_code->co_code;
        unsigned char *code = (unsigned char *)PyBytes_AS_STRING(bytecode);

        if (f->f_lasti < 0) {
            /* Return immediately if the frame didn't start yet. YIELD_FROM
               always come after LOAD_CONST: a code object should not start
               with YIELD_FROM */
            assert(code[0] != YIELD_FROM);
            return NULL;
        }

        if (code[(f->f_lasti+1)*sizeof(_Py_CODEUNIT)] != YIELD_FROM)
            return NULL;
        assert(f->f_stackdepth > 0);
        yf = f->f_valuestack[f->f_stackdepth-1];
        Py_INCREF(yf);
    }

    return yf;
}

static int
gen_is_coroutine(PyObject *o)
{
    if (PyGen_CheckExact(o)) {
        PyCodeObject *code = (PyCodeObject *)((PyGenObject*)o)->gi_code;
        if (code->co_flags & CO_ITERABLE_COROUTINE) {
            return 1;
        }
    }
    return 0;
}

PyObject *
_PyCoro_GetAwaitableIter(PyObject *o)
{
    unaryfunc getter = NULL;
    PyTypeObject *ot;

    if (PyCoro_CheckExact(o) || gen_is_coroutine(o)) {
        /* 'o' is a coroutine. */
        Py_INCREF(o);
        return o;
    }

    ot = Py_TYPE(o);
    if (ot->tp_as_async != NULL) {
        getter = ot->tp_as_async->am_await;
    }
    if (getter != NULL) {
        PyObject *res = (*getter)(o);
        if (res != NULL) {
            if (PyCoro_CheckExact(res) || gen_is_coroutine(res)) {
                /* __await__ must return an *iterator*, not
                   a coroutine or another awaitable (see PEP 492) */
                PyErr_SetString(PyExc_TypeError,
                                "__await__() returned a coroutine");
                Py_CLEAR(res);
            } else if (!PyIter_Check(res)) {
                PyErr_Format(PyExc_TypeError,
                             "__await__() returned non-iterator "
                             "of type '%.100s'",
                             Py_TYPE(res)->tp_name);
                Py_CLEAR(res);
            }
        }
        return res;
    }

    PyErr_Format(PyExc_TypeError,
                 "object %.100s can't be used in 'await' expression",
                 ot->tp_name);
    return NULL;
}

typedef struct _PyAsyncGenWrappedValue {
    PyObject_HEAD
    PyObject *agw_val;
} _PyAsyncGenWrappedValue;

#define _PyAsyncGenWrappedValue_CheckExact(o) \
                    Py_IS_TYPE(o, &_PyAsyncGenWrappedValue_Type)


static struct _Py_async_gen_state *
get_async_gen_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->async_gen;
}

PyObject *
_PyAsyncGenValueWrapperNew(PyObject *val)
{
    _PyAsyncGenWrappedValue *o;
    assert(val);

    struct _Py_async_gen_state *state = get_async_gen_state();
#ifdef Py_DEBUG
    // _PyAsyncGenValueWrapperNew() must not be called after _PyAsyncGen_Fini()
    assert(state->value_numfree != -1);
#endif
    if (state->value_numfree) {
        state->value_numfree--;
        o = state->value_freelist[state->value_numfree];
        assert(_PyAsyncGenWrappedValue_CheckExact(o));
        _Py_NewReference((PyObject*)o);
    }
    else {
        o = PyObject_GC_New(_PyAsyncGenWrappedValue,
                            &_PyAsyncGenWrappedValue_Type);
        if (o == NULL) {
            return NULL;
        }
    }
    o->agw_val = val;
    Py_INCREF(val);
    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}
