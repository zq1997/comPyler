#ifndef COMPYLER_COMPILATION_UNIT
#define COMPYLER_COMPILATION_UNIT

#include <Python.h>
#include <frameobject.h>
#include <opcode.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Host.h>

#include "common.h"
#include "general_utilities.h"
#include "translator.h"
#include "debug_info.h"

inline const llvm::Twine empty_twine;

#ifdef DUMP_DEBUG_FILES
template <typename T>
decltype(auto) useName(const T &arg) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(arg);
    }
    if constexpr (std::is_integral_v<T>) {
        return std::to_string(arg);
    }
    if constexpr (std::is_same_v<T, PyObject *>) {
        std::string result{""};
        if (PyUnicode_Check(arg)) {
            if (auto str = PyUnicode_AsUTF8(arg)) {
                result = str;
            }
        } else {
            if (auto repr_obj = PyObject_Repr(arg)) {
                if (auto str = PyUnicode_AsUTF8(repr_obj)) {
                    result = str;
                }
                Py_DECREF(repr_obj);
            }
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        return result;
    }
}

template <typename T, typename... Ts>
decltype(auto) useName(const T &arg, const Ts &... more) {
    return useName(arg) + useName(more...);
}
#else
#define useName(...) empty_twine
#endif

struct PyAnalysisBlock {
    BitArray locals_touched;
    BitArray locals_set;
    BitArray locals_input;
    union {
        IntVPC _branch_offset;
        PyAnalysisBlock *branch_block;
    };
    PyAnalysisBlock *fall_block;
    PyAnalysisBlock *worklist_link;

    PyAnalysisBlock() = default;

    PyAnalysisBlock(const PyAnalysisBlock &) = delete;

    // It works just like union
    BitArray &getBitArrayForLocalsEverDeleted() { return locals_input; }
};

struct PyCodeBlock : PyAnalysisBlock {
    llvm::BasicBlock *llvm_block;
    llvm::Constant *handler_pc;
    IntVPC begin_vpc;
    int initial_stack_height;
    int stack_effect;
    int branch_stack_difference;
    bool has_try_entrance;
    bool has_try_exit;

    operator llvm::BasicBlock *() { return llvm_block; }

    PyCodeBlock &fall() {
        assert(fall_block);
        return *static_cast<PyCodeBlock *>(fall_block);
    }

    PyCodeBlock &branch() {
        assert(branch_block);
        return *static_cast<PyCodeBlock *>(branch_block);
    }
};

class CompilationUnit {
    friend class BinCodeCache;

    struct AbstractStackValue {
        enum Location { STACK, LOCAL, CONST };
        Location location;
        unsigned index;

        bool on_stack() const { return location == STACK; }

        AbstractStackValue() = default;

        AbstractStackValue(Location location, auto index) : location{location}, index(index) {}
    };

    struct PoppedValue {
        llvm::Value *const value;
        const bool really_pushed;

        PoppedValue(llvm::Value *v, bool p) : value{v}, really_pushed(p) {}

        operator llvm::Value *() { return value; }
    };

    Translator &translator;
    IRBuilder builder{translator.llvm_context};
    llvm::Module llvm_module{"comPyler_module", translator.llvm_context};
    llvm::Function *function;
    llvm::Argument *runtime_symbols;
    llvm::Argument *frame_obj;
    llvm::Argument *cframe;
    llvm::Argument *eval_breaker;
    llvm::BasicBlock *unbound_error_block{nullptr};

    PyCode py_code;
    unsigned block_num;
    DynamicArray<PyCodeBlock> blocks;
    std::unique_ptr<BitArray::ChunkType> analysis_data;
    BitArray redundant_loads;
    unsigned opcache_count{0};
    unsigned handler_num{0};
    DynamicArray<IntVPC> handler_vpc_arr;

    llvm::Value *rt_names;
    llvm::Value *rt_consts;

    int stack_height;
    DynamicArray<uint_least16_t> stack_height_arr;

    DynamicArray<AbstractStackValue> abstract_stack;
    AbstractStackValue *abstract_stack_top;

    void parsePyCode();
    void emitBlock(PyCodeBlock &this_block, DebugInfo &debug_info);
    void emitRotN(PyOparg n);

    llvm::Value *getConst(PyOparg oparg);
    std::pair<llvm::Value *, llvm::Value *> getLocal(PyOparg oparg);
    llvm::Value *getName(PyOparg i);
    llvm::Value *getFreevar(PyOparg i);
    llvm::Value *getStackSlot(PyOparg i = 0);
    llvm::Value *fetchStackValue(PyOparg i);

    PoppedValue pyPop() {
        auto abs_v = *--abstract_stack_top;
        auto really_pushed = abs_v.on_stack();
        stack_height -= really_pushed;
        return {fetchStackValue(0), really_pushed};
    }

    void pyPush(llvm::Value *value) {
        auto &stack_value = *abstract_stack_top++;
        stack_value.location = AbstractStackValue::STACK;
        stack_value.index = stack_height;
        storeValue<PyObject *>(value, getStackSlot(), translator.tbaa_frame_field);
        stack_height++;
    }

    void declareStackGrowth(PyOparg n) {
        for ([[maybe_unused]] auto _ : IntRange(n)) {
            *abstract_stack_top++ = {AbstractStackValue::STACK, stack_height++};
        }
    }

    llvm::Value *declareStackShrink(PyOparg n) {
        abstract_stack_top -= n;
        stack_height -= n;
#ifndef NDEBUG
        for (auto &v : PtrRange(abstract_stack_top, n)) {
            assert(v.on_stack());
        }
#endif
        return getStackSlot();
    }

    void pyIncRef(llvm::Value *v);
    void pyDecRef(llvm::Value *v, bool null_check = false);

    void pyDecRef(PoppedValue &v) {
        if (v.really_pushed) {
            pyDecRef(v.value);
        }
    }

    llvm::Value *getSymbol(size_t index);

    template <auto &Symbol>
    llvm::Value *getSymbol() { return getSymbol(RuntimeSymbols::getIndex<Symbol>()); }

    template <typename T>
    llvm::Constant *getConstantInt(T v) { return llvm::ConstantInt::get(translator.type<T>(), v); }

    template <typename T, typename V>
    llvm::Value *getValueAuto(V v) {
        if constexpr (std::is_integral_v<V> || std::is_enum_v<V>) {
            return getConstantInt<T>(v);
        } else {
            return static_cast<llvm::Value *>(v);
        }
    }

    auto createBlock(llvm::Function *parent, const char *name = "") {
        return llvm::BasicBlock::Create(translator.llvm_context, useName(name), parent);
    }

    template <typename T=char>
    auto calcElementAddr(llvm::Value *base, ptrdiff_t index) {
        return builder.CreateInBoundsGEP(translator.type<char>(), base, getConstantInt(sizeof(T) * index));
    }

    template <typename T, typename M>
    auto calcFieldAddr(llvm::Value *instance, M T::* member) {
        T dummy;
        auto offset = reinterpret_cast<const char *>(&(dummy.*member)) - reinterpret_cast<const char *>(&dummy);
        return calcElementAddr(instance, offset);
    }

    template <typename T>
    llvm::Value *loadValue(llvm::Value *ptr, llvm::MDNode *tbaa, const llvm::Twine &name = empty_twine) {
        auto load_inst = builder.CreateLoad(translator.type<T>(), ptr, name);
        load_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
        return load_inst;
    }

    template <typename T>
    void storeValue(auto value, llvm::Value *ptr, llvm::MDNode *tbaa) {
        auto store_inst = builder.CreateStore(getValueAuto<T>(value), ptr);
        store_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
    }

    template <typename T, typename M>
    auto loadFieldValue(llvm::Value *instance, M T::* member, llvm::MDNode *tbaa) {
        return loadValue<M>(calcFieldAddr(instance, member), tbaa);
    }

    template <typename T, typename M>
    auto storeFieldValue(auto value, llvm::Value *instance, M T::* member, llvm::MDNode *tbaa) {
        return storeValue<M>(value, calcFieldAddr(instance, member), tbaa);
    }

    void returnFrame(PyFrameState state, llvm::Value *retval) {
        storeFieldValue(state, frame_obj, &PyFrameObject::f_state, translator.tbaa_frame_field);
        storeFieldValue(stack_height, frame_obj, &PyFrameObject::f_stackdepth, translator.tbaa_frame_field);
        builder.CreateRet(retval);
    }

    template <typename Ret, typename... Args>
    auto emitCallImpl(Ret (&)(Args...), llvm::Value *callee, auto &&... args) {
        return builder.CreateCall(translator.type<Ret(Args...)>(), callee, {getValueAuto<Args>(args)...});
    }

    template <auto &Symbol>
    auto emitCall(auto &&... args) { return emitCallImpl(Symbol, getSymbol<Symbol>(), args...); }

    void declareBlockAsHandler(PyCodeBlock &block);

    void emitUnlikelyJump(llvm::Value *cond, llvm::BasicBlock *jump_block, const char *fall_block_name) {
        auto fall_block = createBlock(function, fall_block_name);
        builder.CreateCondBr(cond, jump_block, fall_block, translator.unlikely);
        builder.SetInsertPoint(fall_block);
    }

    void checkUnboundError(llvm::Value *value) {
        if (!unbound_error_block) {
            unbound_error_block = createBlock(nullptr, "UnboundError");
        }
        emitUnlikelyJump(builder.CreateICmpEQ(value, translator.c_null), unbound_error_block, "load_ok");
    }

    void emitConditionalJump(llvm::Value *value, bool cond, llvm::BasicBlock *branch, llvm::BasicBlock *fall);

    template <PyObject *(&Symbol)(PyObject *)>
    void emitUnaryOperation() {
        auto value = pyPop();
        auto res = emitCall<Symbol>(value);
        pyPush(res);
        pyDecRef(value);
    }

    template <PyObject *(&Symbol)(PyObject *, PyObject *)>
    void emitBinaryOperation() {
        auto right = pyPop();
        auto left = pyPop();
        auto res = emitCall<Symbol>(left, right);
        pyPush(res);
        pyDecRef(left);
        pyDecRef(right);
    }

    void emitCheckEvalBreaker(IntVPC next_vpc);

public:
    explicit CompilationUnit(Translator &translator, PyCode py_code) : translator{translator}, py_code{py_code} {};
    bool translate(PyObject *debug_args);
};

class BinCodeCache {
public:
    struct CacheMeta {
        long long timestamp;
        unsigned long long hash;
        unsigned py_code_size;
        unsigned bin_code_size;
        unsigned rodata_size;
        unsigned opcache_num;
        unsigned handler_num;
    };
private:
    static constexpr char BINARY_CACHE_SUFFIX[]{".compyler-310.bin"};
    static constexpr uint64_t HASH_SEED{310};

    inline static llvm::SmallString<512> cache_root;

    PyCode py_code;
    unsigned long long instr_hash;
    long long src_timestamp;
    int fd{-1};
    llvm::sys::fs::file_t native_file_handler;
    size_t append_at{0};
    bool is_anonymous_code{false};

    bool isCacheEnabled() { return fd != -1; }

public:
    BinCodeCache(PyCode py_code);
    ~BinCodeCache();
    TranslatedResult *load();
    TranslatedResult *store(CompilationUnit &cu);

    static void setCacheRoot(const char *root);
};

#endif
