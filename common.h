#ifndef COMPYLER_COMMON_H
#define COMPYLER_COMMON_H

#include <Python.h>

#if defined(Py_REF_DEBUG) || defined(Py_TRACE_REFS)
#define NON_INLINE_RC
#endif

#ifndef NDEBUG
#define DUMP_DEBUG_FILES
#endif

#endif
