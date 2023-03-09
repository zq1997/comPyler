#include "Python.h"
#include "dict-common.h"


#define DK_SIZE(dk) ((dk)->dk_size)
#if SIZEOF_VOID_P > 4
#define DK_IXSIZE(dk)                          \
    (DK_SIZE(dk) <= 0xff ?                     \
        1 : DK_SIZE(dk) <= 0xffff ?            \
            2 : DK_SIZE(dk) <= 0xffffffff ?    \
                4 : sizeof(int64_t))
#else
#define DK_IXSIZE(dk)                          \
    (DK_SIZE(dk) <= 0xff ?                     \
        1 : DK_SIZE(dk) <= 0xffff ?            \
            2 : sizeof(int32_t))
#endif
#define DK_ENTRIES(dk) \
    ((PyDictKeyEntry*)(&((int8_t*)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))

Py_ssize_t
_PyDict_GetItemHint(PyDictObject *mp, PyObject *key,
        Py_ssize_t hint, PyObject **value)
{
    assert(*value == NULL);
    assert(PyDict_CheckExact((PyObject*)mp));
    assert(PyUnicode_CheckExact(key));

    if (hint >= 0 && hint < mp->ma_keys->dk_nentries) {
        PyObject *res = NULL;

        PyDictKeyEntry *ep = DK_ENTRIES(mp->ma_keys) + (size_t)hint;
        if (ep->me_key == key) {
            if (mp->ma_values) {
                res = mp->ma_values[(size_t)hint];
            }
            else {
                res = ep->me_value;
            }
            if (res != NULL) {
                *value = res;
                return hint;
            }
        }
    }

    Py_hash_t hash = ((PyASCIIObject *) key)->hash;
    if (hash == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1) {
            return -1;
        }
    }

    return (mp->ma_keys->dk_lookup)(mp, key, hash, value);
}

PyObject *
_PyDict_LoadGlobal(PyDictObject *globals, PyDictObject *builtins, PyObject *key)
{
    Py_ssize_t ix;
    Py_hash_t hash;
    PyObject *value;

    if (!PyUnicode_CheckExact(key) ||
            (hash = ((PyASCIIObject *) key)->hash) == -1)
    {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }

    /* namespace 1: globals */
    ix = globals->ma_keys->dk_lookup(globals, key, hash, &value);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix != DKIX_EMPTY && value != NULL)
        return value;

    /* namespace 2: builtins */
    ix = builtins->ma_keys->dk_lookup(builtins, key, hash, &value);
    if (ix < 0)
        return NULL;
    return value;
}
