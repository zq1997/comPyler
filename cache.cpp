#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "general_utilities.h"
#include "translated_result.h"
#include "compilation_unit.h"

struct SpaceCalculator {
    size_t size{0};

    template <typename T>
    size_t appendElements(size_t n) {
        constexpr auto align = alignof(T);
        size_t offset = (size + align - 1) / align * align;
        size = offset + sizeof(T) * n;
        return offset;
    }
};

static TranslatedResult *allocateSpaceForResult(BinCodeCache::CacheMeta &meta, PyCode py_code) {
    SpaceCalculator calculator;
    calculator.appendElements<TranslatedResult>(1);
    auto offset_opcache = calculator.appendElements<_PyOpcache>(meta.opcache_num);
    auto offset_vpc = calculator.appendElements<IntVPC>(meta.handler_num);
    auto offset_pc = calculator.appendElements<IntPC>(meta.handler_num);
    auto offset_stack_height_arr = py_code->co_stacksize <= UINT_LEAST8_MAX ?
            calculator.appendElements<uint_least8_t>(py_code.instrNum()) :
            calculator.appendElements<uint_least16_t>(py_code.instrNum());

    assert(meta.rodata_size == 0 || meta.rodata_size == calculator.size - offset_vpc);
    meta.rodata_size = calculator.size - offset_vpc;

    auto buffer = TranslatedResult::create(meta.bin_code_size, calculator.size);
    auto result = reinterpret_cast<TranslatedResult *>(buffer);
    result->handler_num = meta.handler_num;
    result->opcache_arr = reinterpret_cast<_PyOpcache *>(&buffer[offset_opcache]);
    result->handler_vpc_arr = reinterpret_cast<IntVPC *>(&buffer[offset_vpc]);
    result->handler_pc_arr = reinterpret_cast<IntPC *>(&buffer[offset_pc]);
    result->stack_height_arr = &buffer[offset_stack_height_arr];

    for (auto &opcache : PtrRange(result->opcache_arr, meta.opcache_num)) {
        opcache.optimized = 0;
    }
    return result;
}

static void *getRodata(TranslatedResult *result) {
    return result->handler_vpc_arr;
}

void BinCodeCache::setCacheRoot(const char *root) {
    cache_root = root;
    if (!cache_root.size() || llvm::sys::fs::make_absolute(cache_root)) {
        cache_root.clear();
    }
}

BinCodeCache::BinCodeCache(PyCode py_code) : py_code{py_code} {
    if (!cache_root.size()) {
        return;
    }

    instr_hash = llvm::hashing::detail::hash_short(
            reinterpret_cast<char *>(py_code.instrData()), py_code.instrSize(), HASH_SEED);

    auto src_path_obj = PyUnicode_EncodeFSDefault(py_code->co_filename);
    if (!src_path_obj) {
        PyErr_Clear();
        return;
    }

    llvm::StringRef src_path{
            PyBytes_AS_STRING(src_path_obj),
            static_cast<size_t>(PyBytes_GET_SIZE(src_path_obj))
    };
    llvm::sys::fs::file_status src_status;
    if (llvm::sys::fs::status(src_path, src_status, true)) {
        if (src_path.startswith("<frozen ") && src_path.endswith(">")) {
            src_timestamp = std::numeric_limits<decltype(src_timestamp)>::min();
        } else {
            return;
        }
    } else {
        src_timestamp = src_status.getLastModificationTime().time_since_epoch().count();
    }
    auto cache_path{cache_root};
    llvm::sys::path::append(cache_path, src_path);
    Py_DECREF(src_path_obj);
    llvm::sys::path::remove_dots(cache_path, true);

    cache_path.push_back('@');
    cache_path.append(std::to_string(py_code->co_firstlineno));
    if (PyUnicode_READ_CHAR(py_code->co_name, 0) == '<') {
        if (PyUnicode_CompareWithASCIIString(py_code->co_name, "<module>") == 0) {
            cache_path.append("~mod");
        } else {
            is_anonymous_code = true;
            cache_path.append("~anno");
        }
    }
    cache_path.append(BINARY_CACHE_SUFFIX);

    if (llvm::sys::fs::create_directories(llvm::sys::path::parent_path(cache_path))) {
        return;
    }

    int opened_fd;
    if (llvm::sys::fs::openFileForReadWrite(cache_path, opened_fd,
            llvm::sys::fs::CreationDisposition::CD_OpenAlways,
            llvm::sys::fs::OpenFlags::OF_None)) {
        return;
    }
    native_file_handler = llvm::sys::fs::convertFDToNativeFile(opened_fd);
    if (llvm::sys::fs::lockFile(opened_fd)) {
        llvm::sys::fs::closeFile(native_file_handler);
        return;
    }
    fd = opened_fd;
}

BinCodeCache::~BinCodeCache() {
    if (isCacheEnabled()) {
        llvm::sys::fs::closeFile(native_file_handler);
    }
}

TranslatedResult *BinCodeCache::load() {
    if (!isCacheEnabled()) {
        return nullptr;
    }

    bool read_fully = true;
    bool read_eof;
    size_t offset{0};
    const auto &readFile = [&](void *buffer, size_t size) {
        if (!read_fully) {
            return;
        }
        auto read_result = llvm::sys::fs::readNativeFileSlice(native_file_handler,
                llvm::MutableArrayRef{reinterpret_cast<char *>(buffer), size}, offset);
        if (read_result) {
            read_fully = *read_result == size;
            read_eof = *read_result == 0;
            offset += *read_result;
        } else {
            read_fully = read_eof = false;
        }
    };


    while (true) {
        CacheMeta meta;
        readFile(&meta, sizeof(meta));
        if (!read_fully) {
            if (read_eof && is_anonymous_code) {
                append_at = offset;
            }
            return nullptr;
        }
        if (meta.timestamp != src_timestamp) {
            return nullptr;
        }

        if (is_anonymous_code) {
            if (meta.py_code_size != py_code.instrSize() || meta.hash != instr_hash) {
                offset += meta.py_code_size + meta.bin_code_size + meta.rodata_size;
                continue;
            }
            DynamicArray<char> py_instr_buffer(meta.py_code_size);
            readFile(py_instr_buffer + 0, meta.py_code_size);
            if (memcmp(py_instr_buffer + 0, py_code.instrData(), meta.py_code_size)) {
                offset += meta.bin_code_size + meta.rodata_size;
                continue;
            }
        } else {
            if (meta.py_code_size != py_code.instrSize() || meta.hash != instr_hash) {
                return nullptr;
            }
        }

        auto result = allocateSpaceForResult(meta, py_code);
        if (!result) {
            PyErr_Clear();
            return nullptr;
        }
        readFile(result->entry_address(), meta.bin_code_size);
        readFile(getRodata(result), meta.rodata_size);
        if (!read_fully) {
            return nullptr;
        }
        return result;
    };
}

TranslatedResult *BinCodeCache::store(CompilationUnit &cu) {
    auto text_section = cu.translator.getTextSection();
    CacheMeta meta;
    meta.timestamp = src_timestamp;
    meta.hash = instr_hash;
    meta.py_code_size = py_code.instrSize();
    meta.bin_code_size = text_section.size();
    meta.rodata_size = 0;
    meta.opcache_num = cu.opcache_count;
    meta.handler_num = cu.handler_num;
    auto result = allocateSpaceForResult(meta, py_code);
    if (!result) {
        return nullptr;
    }

    if (py_code->co_stacksize <= UINT_LEAST8_MAX) {
        std::transform(cu.stack_height_arr + 0, cu.stack_height_arr + py_code.instrNum(),
                reinterpret_cast<uint_least8_t *>(result->stack_height_arr),
                std::identity());
    } else {
        memcpy(result->stack_height_arr, cu.stack_height_arr, py_code.instrNum() * sizeof(uint_least16_t));
    }
    memcpy(result->entry_address(), text_section.data(), meta.bin_code_size);
    if (cu.handler_num) {
        memcpy(result->handler_vpc_arr, cu.handler_vpc_arr, sizeof(IntVPC) * cu.handler_num);
        assert(cu.translator.getDataSection().size() == sizeof(IntPC) * cu.handler_num);
        memcpy(result->handler_pc_arr, cu.translator.getDataSection().data(), cu.translator.getDataSection().size());
    }

    if (!isCacheEnabled()) {
        return result;
    }

    llvm::raw_fd_ostream fd_os{fd, false};
    fd_os.seek(append_at);
    fd_os.write(reinterpret_cast<char *>(&meta), sizeof(meta));
    if (is_anonymous_code) {
        fd_os.write(reinterpret_cast<char *>(py_code.instrData()), meta.py_code_size);
    }
    fd_os.write(text_section.data(), meta.bin_code_size);
    fd_os.write(reinterpret_cast<char *>(getRodata(result)), meta.rodata_size);
    // Note: We'd better write the magic number last

    llvm::sys::fs::resize_file(fd, fd_os.has_error() ? 0 : fd_os.tell());
    return result;
}
