#ifndef COMPYLER_TRANSLATED_RESULT_H
#define COMPYLER_TRANSLATED_RESULT_H

#include <csetjmp>

#include <Python.h>
#include <internal/pycore_code.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Memory.h>

#include "general_utilities.h"

inline Py_ssize_t code_extra_index;
inline PyObject *compyler_module{nullptr};

struct ExtendedCFrame : CFrame {
    const struct TranslatedResult *translated_result;
    IntPC handler;
    jmp_buf frame_jmp_buf;
};

using TargetFunction = PyObject *(void *const[], PyFrameObject *, ExtendedCFrame *cframe, void *);

struct ExeMemBlock {
    llvm::sys::MemoryBlock llvm_mem_block;
    ExeMemBlock *left;
    ExeMemBlock *right;
    size_t remaining_size;
    size_t fragment_in_use;

    void remove() {
        assert(left != this && right != this);
        left->right = right;
        right->left = left;
    }

    void insertBetween(ExeMemBlock *l, ExeMemBlock *r) {
        left = l;
        right = r;
        l->right = this;
        r->left = this;
    }

    static void clear();
};

struct TranslatedResult {
    void *exe_addr;
    ExeMemBlock *exe_mem_block;
    unsigned handler_num;
    _PyOpcache *opcache_arr;
    IntVPC *handler_vpc_arr;
    IntPC *handler_pc_arr;
    void *stack_height_arr;

    void *entry_address() const { return exe_addr; }

    auto operator()(auto ...args) const {
        auto binary_func = reinterpret_cast<TargetFunction *>(entry_address());
        return binary_func(args...);
    }

    IntPC calcPC(IntVPC vpc) const {
        auto ptr = std::lower_bound(handler_vpc_arr, handler_vpc_arr + handler_num, vpc);
        assert(ptr != handler_vpc_arr + handler_num && *ptr == vpc);
        return handler_pc_arr[ptr - handler_vpc_arr];
    }

    static char *create(size_t bin_code_size, size_t buffer_size);
    static void destroy(void *buffer);
};

typedef struct {
    Py_ssize_t ce_size;
    void *ce_extras[1];
} _PyCodeObjectExtra;

inline bool hasTranslatedResult(PyCodeObject *co) {
    auto co_extra = reinterpret_cast<_PyCodeObjectExtra *>(co->co_extra);
    return co_extra && code_extra_index < co_extra->ce_size;
}

inline auto &getTranslatedResult(PyCodeObject *co) {
    auto co_extra = reinterpret_cast<_PyCodeObjectExtra *>(co->co_extra);
    return *reinterpret_cast<TranslatedResult *>(co_extra->ce_extras[code_extra_index]);
}

#endif
