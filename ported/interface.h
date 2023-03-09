#ifndef COMPYLER_INTERFACE_H
#define COMPYLER_INTERFACE_H

#include <Python.h>

extern int jit_threshold;

#ifdef __cplusplus
extern "C" {
#endif

PyObject *Ported_PyEval_EvalFrameDefault(PyThreadState *tstate, PyFrameObject *f, int throwflag);
PyObject *tackOverFrame(PyThreadState *tstate, PyFrameObject *f, int vpc);
int eval_frame_handle_pending(PyThreadState *tstate);

extern PyObject compilation_error;

#ifdef __cplusplus
}
#endif
#endif
