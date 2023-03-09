#ifndef COMPYLER_TRANSLATOR_H
#define COMPYLER_TRANSLATOR_H

#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Support/Host.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/SubtargetFeature.h>

#ifdef ABLATION_BUILD
#include <llvm/Passes/PassBuilder.h>
#endif

#include "common.h"
#include "types.h"
#include "general_utilities.h"

class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine;

#ifdef ABLATION_BUILD
    llvm::ModuleAnalysisManager opt_MAM;
    llvm::CGSCCAnalysisManager opt_CGAM;
    llvm::FunctionAnalysisManager opt_FAM;
    llvm::LoopAnalysisManager opt_LAM;
    llvm::ModulePassManager opt_MPM;
#endif

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec;
    llvm::raw_svector_ostream out_stream{out_vec};
    llvm::StringRef text_section;
    llvm::StringRef data_section;

public:
    bool initialize();

    auto createDataLayout() { return machine->createDataLayout(); }

    bool compile(llvm::Module &mod);

    auto &getObjectContent() { return out_vec; }

    auto getTextSection() { return text_section; }

    auto getDataSection() { return data_section; }
};

class Context {
    template <typename T>
    struct LLVMTypeGetter {
        static llvm::Type *get(llvm::LLVMContext &llvm_context) {
            if constexpr (std::is_void_v<T>) {
                return llvm::Type::getVoidTy(llvm_context);
            }
            if constexpr (std::is_integral_v<T>) {
                if (std::is_same_v<T, bool>) {
                    return llvm::Type::getInt1Ty(llvm_context);
                } else {
                    return llvm::Type::getIntNTy(llvm_context, CHAR_BIT * sizeof(T));
                }
            }
        }
    };

    template <typename T>
    struct LLVMTypeGetter<T *> {
        static llvm::PointerType *get(llvm::LLVMContext &llvm_context) {
            return llvm::PointerType::getUnqual(llvm_context);
        }
    };

    template <typename Ret, typename... Args>
    struct LLVMTypeGetter<Ret(Args...)> {
        static llvm::FunctionType *get(llvm::LLVMContext &llvm_context) {
            return llvm::FunctionType::get(
                    LLVMTypeGetter<Ret>::get(llvm_context),
                    {LLVMTypeGetter<Args>::get(llvm_context)...},
                    false
            );
        }
    };


    template <typename T>
    static auto getLLVMType(llvm::LLVMContext &llvm_context) {
        static decltype(LLVMTypeGetter<T>::get(llvm_context)) result = nullptr;
        if (!result) {
            result = LLVMTypeGetter<T>::get(llvm_context);
        }
        return result;
    }
public:
    llvm::LLVMContext llvm_context;
    llvm::Constant *c_null;
    llvm::MDNode *unlikely;
    llvm::MDNode *tbaa_refcnt;
    llvm::MDNode *tbaa_obj_field;
    llvm::MDNode *tbaa_frame_field;
    llvm::MDNode *tbaa_immutable;

    Context();

    template <typename T>
    auto type() { return getLLVMType<NormalizedType<T>>(llvm_context); }
};

class IRInserterImpl : public llvm::IRBuilderDefaultInserter {
public:
    virtual void InsertHelper(llvm::Instruction *inst, const llvm::Twine &name,
            llvm::BasicBlock *block, llvm::BasicBlock::iterator where) const {
        assert(block);
        inst->setName(name);
        block->getInstList().insert(where, inst);
    }

    void setDebugLocation(llvm::DILocation *location) {}
};

class IRInserterImplDebug : public IRInserterImpl {
    llvm::DILocation *di_location{nullptr};
public:
    virtual void InsertHelper(llvm::Instruction *inst, const llvm::Twine &name,
            llvm::BasicBlock *block, llvm::BasicBlock::iterator where) const {
        this->IRInserterImpl::InsertHelper(inst, name, block, where);
        if (di_location) {
            inst->setMetadata(llvm::LLVMContext::MD_dbg, di_location);
        }
    }

    void setDebugLocation(llvm::DILocation *location) {
        di_location = location;
    }
};


#ifdef DUMP_DEBUG_FILES
using IRBuilder = llvm::IRBuilder<llvm::ConstantFolder, IRInserterImplDebug>;
#else
using IRBuilder = llvm::IRBuilder<llvm::ConstantFolder, IRInserterImpl>;
#endif

class Translator : public Context, public Compiler {};

#ifdef ABLATION_BUILD
inline bool with_SOE = true;
inline bool with_ICE = true;
inline bool with_IRO = false;
#else
constexpr bool with_SOE = true;
constexpr bool with_ICE = true;
#endif

#endif
