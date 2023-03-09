#ifndef COMPYLER_GENERAL_UTILITIES
#define COMPYLER_GENERAL_UTILITIES

#include <Python.h>
#include <opcode.h>

#include "common.h"

template <typename T>
class DynamicArray {
    T *data{};
#ifndef NDEBUG
    size_t array_size;
#endif

public:
    DynamicArray() = default;
    DynamicArray(const DynamicArray &) = delete;

    explicit DynamicArray(size_t size) { reserve(size); };

    ~DynamicArray() {
        if (data) {
            delete[] data;
        }
    };

    void reserve(size_t size) {
        assert(!data);
        data = new T[size];
#ifndef NDEBUG
        array_size = size;
#endif
    }

    operator T *() { return data; }

    T *operator+(size_t offset) {
        return data + offset;
    }

    T &operator[](size_t index) {
        assert(index < array_size);
        return data[index];
    }
};

class BitArray {
public:
    using ChunkType = uintmax_t;
protected:
    uintmax_t *data;
public:
    static constexpr auto bits_per_chunk = CHAR_BIT * sizeof(ChunkType);
    using Parent = DynamicArray<ChunkType>;

    static auto chunkNumber(size_t size) { return size / bits_per_chunk + !!(size % bits_per_chunk); }

    void usePreallocatedSpace(ChunkType *&space, size_t size) {
        data = space;
        space += size;
    }

    bool testAndSet(size_t index) {
        auto &chunk = data[index / bits_per_chunk];
        auto old_chunk = chunk;
        auto tester = ChunkType{1} << index % bits_per_chunk;
        chunk |= tester;
        return !(old_chunk & tester);
    }

    void reset(size_t index) {
        data[index / bits_per_chunk] &= ~(ChunkType{1} << index % bits_per_chunk);
    }

    void setIf(size_t index, bool cond) {
        data[index / bits_per_chunk] |= ChunkType{cond} << index % bits_per_chunk;
    }

    void set(size_t index) {
        setIf(index, true);
    }

    bool get(size_t index) {
        return data[index / bits_per_chunk] & (ChunkType{1} << index % bits_per_chunk);
    }

    ChunkType &operator[](size_t index) {
        return data[index];
    }
};

class ManagedBitArray : public BitArray {
public:
    explicit ManagedBitArray(size_t size) { data = new ChunkType[chunkNumber(size)]{}; }

    ManagedBitArray(const ManagedBitArray &) = delete;

    ~ManagedBitArray() { delete[] data; }
};

using IntVPC = int;
using IntPC = int;
using PyOparg = uint_fast32_t;

class PyCode {
    PyCodeObject *co;
public:
    PyCode(PyCodeObject *co) : co(co) {}

    PyCode(PyObject *co) : co(reinterpret_cast<PyCodeObject *>(co)) {}

    operator PyCodeObject *() { return co; }

    operator PyObject *() { return reinterpret_cast<PyObject *>(co); }

    static constexpr unsigned extended_arg_shift = 8;

    auto instrData() { return reinterpret_cast<_Py_CODEUNIT *>(PyBytes_AS_STRING(co->co_code)); }

    auto instrSize() { return PyBytes_GET_SIZE(co->co_code); }

    IntVPC instrNum() { return instrSize() / sizeof(_Py_CODEUNIT); }

    auto operator->() { return co; }
};

template <typename T1, typename T2 = T1>
class IntRange {
    using T = std::common_type_t<T1, T2>;
    static_assert(std::is_integral_v<T>);
    const T from;
    const T to;

    class Iterator {
        T i;
    public:
        explicit Iterator(T i) : i{i} {}

        auto &operator++() {
            i += 1;
            return *this;
        }

        auto operator!=(const Iterator &o) const { return o.i != i; }

        auto &operator*() const { return i; }
    };

public:
    IntRange(T1 from, T2 to) : from(from), to(to) {}

    explicit IntRange(T1 n) : from{0}, to{n} {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

template <typename T>
class PtrRange {
    T *const from;
    T *const to;

    class Iterator {
        T *i;
    public:
        explicit Iterator(T *i) : i{i} {}

        auto &operator++() {
            i += 1;
            return *this;
        }

        auto operator!=(const Iterator &o) const { return o.i != i; }

        auto &operator*() const { return *i; }
    };

public:
    PtrRange(T *base, size_t n) : from{base}, to{base + n} {}

    PtrRange(DynamicArray<T> &arr, size_t n) : PtrRange{static_cast<T *>(arr), n} {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

#endif
