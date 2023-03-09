#ifndef COMPYLER_SHARED_SYMBOLS
#define COMPYLER_SHARED_SYMBOLS

#include <csetjmp>

#include <Python.h>
#undef HAVE_STD_ATOMIC
#include <frameobject.h>
#include <internal/pycore_code.h>
#include <internal/pycore_atomic.h>

#include "common.h"

void raiseUndefinedName(PyThreadState *tstate, PyObject *name, bool is_free_var = false);
[[noreturn]] void raiseUnboundError();

void handle_ROT_N(PyObject **values, Py_ssize_t n_lift);

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, Py_ssize_t index);
PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyObject *name, _PyOpcache *co_opcache);
void handle_STORE_GLOBAL(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_GLOBAL(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_NAME(PyFrameObject *f, PyObject *name);
void handle_STORE_NAME(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_NAME(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_ATTR(PyObject *owner, PyObject *name, PyFrameObject *f, _PyOpcache *co_opcache);
void handle_LOAD_METHOD(PyObject *name, PyObject **sp);
void handle_STORE_ATTR(PyObject *owner, PyObject *name, PyObject *value);
PyObject *handle_BINARY_SUBSCR(PyObject *container, PyObject *sub);
void handle_STORE_SUBSCR(PyObject *container, PyObject *sub, PyObject *value);
void handle_DELETE_SUBSCR(PyObject *container, PyObject *sub);

PyObject *handle_UNARY_NOT(PyObject *value);
PyObject *handle_UNARY_POSITIVE(PyObject *value);
PyObject *handle_UNARY_NEGATIVE(PyObject *value);
PyObject *handle_UNARY_INVERT(PyObject *value);

PyObject *handle_BINARY_ADD(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_ADD(PyObject *v, PyObject *w);
PyObject *handle_BINARY_SUBTRACT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_SUBTRACT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_BINARY_FLOOR_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_FLOOR_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_BINARY_TRUE_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_TRUE_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MODULO(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MODULO(PyObject *v, PyObject *w);
PyObject *handle_BINARY_POWER(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_POWER(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MATRIX_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MATRIX_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_BINARY_LSHIFT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_LSHIFT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_RSHIFT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_RSHIFT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_AND(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_AND(PyObject *v, PyObject *w);
PyObject *handle_BINARY_OR(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_OR(PyObject *v, PyObject *w);
PyObject *handle_BINARY_XOR(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_XOR(PyObject *v, PyObject *w);
PyObject *handle_COMPARE_OP(PyObject *v, PyObject *w, int op);
PyObject *handle_CONTAINS_OP(PyObject *value, PyObject *container, bool invert);

PyObject *handle_CALL_FUNCTION(PyObject **func_args, Py_ssize_t nargs);
PyObject *handle_CALL_METHOD(PyObject **func_args, Py_ssize_t nargs);
PyObject *handle_CALL_FUNCTION_KW(PyObject **func_args, Py_ssize_t nargs, PyObject *kwnames);
PyObject *handle_CALL_FUNCTION_EX(PyObject *func, PyObject *args, PyObject *kwargs);
PyObject *handle_MAKE_FUNCTION(PyObject *codeobj, PyFrameObject *f, PyObject *qualname,
        PyObject **extra, int flag);
PyObject *handle_LOAD_BUILD_CLASS(PyFrameObject *f);

PyObject *handle_IMPORT_NAME(PyFrameObject *f, PyObject *name, PyObject *fromlist, PyObject *level);
PyObject *handle_IMPORT_FROM(PyObject *from, PyObject *name);
void handle_IMPORT_STAR(PyFrameObject *f, PyObject *from);

PyObject *handle_GET_ITER(PyObject *o);
void handle_FOR_ITER(PyObject *iter);

PyObject *handle_BUILD_STRING(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_TUPLE(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_LIST(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_SET(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_MAP(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_CONST_KEY_MAP(PyObject **arr, Py_ssize_t num);
void handle_LIST_APPEND(PyObject *list, PyObject *value);
void handle_SET_ADD(PyObject *set, PyObject *value);
void handle_MAP_ADD(PyObject *map, PyObject *key, PyObject *value);
void handle_LIST_EXTEND(PyObject *list, PyObject *iterable);
void handle_SET_UPDATE(PyObject *set, PyObject *iterable);
void handle_DICT_UPDATE(PyObject *dict, PyObject *update);
void handle_DICT_MERGE(PyObject *func, PyObject *dict, PyObject *update);
PyObject *handle_LIST_TO_TUPLE(PyObject *list);

PyObject *handle_FORMAT_VALUE(PyObject *value, PyObject *fmt_spec, int which_conversion);
PyObject *handle_BUILD_SLICE(PyObject *start, PyObject *stop, PyObject *step);
void handle_SETUP_ANNOTATIONS(PyFrameObject *f);
void handle_PRINT_EXPR(PyObject *value);

void handle_UNPACK_SEQUENCE(PyObject *seq, Py_ssize_t num, PyObject **dest);
void handle_UNPACK_EX(PyObject *seq, Py_ssize_t before_star, Py_ssize_t after_star, PyObject **dest);

PyObject *hanlde_GET_LEN(PyObject *value);
void hanlde_MATCH_KEYS(PyObject **inputs_outputs);
void hanlde_MATCH_CLASS(Py_ssize_t nargs, PyObject *kwargs, PyObject **inputs_outputs);
PyObject *handle_COPY_DICT_WITHOUT_KEYS(PyObject *subject, PyObject *keys);

void handle_POP_EXCEPT(PyFrameObject *f, PyObject **exc_triple);
bool handle_JUMP_IF_NOT_EXC_MATCH(PyObject *left, PyObject *right);
[[noreturn]] void handle_RERAISE(bool restore_lasti, int stack_height);
void handle_SETUP_WITH(PyFrameObject *f, int handler, int stack_height);
PyObject *handle_WITH_EXCEPT_START(PyObject **stack_top);
[[noreturn]] void handle_RAISE_VARARGS(int argc, int stack_height);

PyObject *handle_YIELD_VALUE(PyObject *val);
PyObject *handle_YIELD_FROM(PyObject **inputs_outputs);
PyObject *handle_GET_YIELD_FROM_ITER(PyObject *iterable, bool is_coroutine);
PyObject *handle_GET_AWAITABLE(PyObject *iterable, int prev_prev_op, int prev_op);
PyObject *handle_GET_AITER(PyObject *obj);
PyObject *handle_GET_ANEXT(PyObject *aiter);
void handle_END_ASYNC_FOR(int stack_height);
void handle_BEFORE_ASYNC_WITH(PyObject **sp);

bool castPyObjectToBool(PyObject *o);

void handleEvalBreaker();

#endif
