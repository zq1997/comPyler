#include "translator.h"

using namespace llvm;

constexpr auto BIN_CODE_ALIGNMENT{alignof(std::max_align_t)};
constexpr auto MIN_FRAGMENT_SIZE{256};
constexpr auto MIN_BLOCK_SIZE{64 * (1 << 10)};

static ExeMemBlock mem_blocks{
        .left = &mem_blocks,
        .right = &mem_blocks,
        .remaining_size = 0
};

void ExeMemBlock::clear() {
    for (auto block = mem_blocks.right; block != &mem_blocks;) {
        assert(!block->fragment_in_use);
        sys::Memory::releaseMappedMemory(block->llvm_mem_block);
        auto next_block = block->right;
        delete block;
        block = next_block;
    }
}

char *TranslatedResult::create(size_t bin_code_size, size_t buffer_size) {
    bin_code_size = ((bin_code_size - 1) / BIN_CODE_ALIGNMENT + 1) * BIN_CODE_ALIGNMENT;

    ExeMemBlock *block;
    for (block = mem_blocks.right; block->remaining_size > 0; block = block->right) {
        if (block->remaining_size >= bin_code_size) {
            break;
        }
    }
    if (!block->remaining_size) {
        std::error_code ec;
        auto &&llvm_mem_block = llvm::sys::Memory::allocateMappedMemory(
                std::max<size_t>(bin_code_size, MIN_BLOCK_SIZE),
                nullptr,
                llvm::sys::Memory::ProtectionFlags::MF_RWE_MASK, ec);
        if (ec) {
            PyErr_SetString(PyExc_SystemError, ec.message().c_str());
            return nullptr;
        }

        block = new ExeMemBlock{
                .llvm_mem_block = llvm_mem_block,
                .remaining_size = llvm_mem_block.allocatedSize(),
                .fragment_in_use = 0
        };
        block->insertBetween(&mem_blocks, mem_blocks.right);
    }

    assert(block->remaining_size >= bin_code_size);
    auto buffer = new char[buffer_size];
    auto result = reinterpret_cast<TranslatedResult *>(buffer);
    result->exe_addr = reinterpret_cast<char *>(block->llvm_mem_block.base())
            + (block->llvm_mem_block.allocatedSize() - block->remaining_size);
    result->exe_mem_block = block;
    block->remaining_size -= bin_code_size;
    block->fragment_in_use++;

    if (block->remaining_size < MIN_FRAGMENT_SIZE) {
        block->remaining_size = 0;
        block->remove();
        block->insertBetween(mem_blocks.left, &mem_blocks);
    }

    Py_INCREF(compyler_module);
    return buffer;
}

void TranslatedResult::destroy(void *buffer) {
    if (buffer) {
        auto block = reinterpret_cast<TranslatedResult *>(buffer)->exe_mem_block;
        if (!--block->fragment_in_use) {
            block->remove();
            block->insertBetween(&mem_blocks, mem_blocks.right);
            block->remaining_size = block->llvm_mem_block.allocatedSize();
        }
        Py_DECREF(compyler_module);
        delete[] reinterpret_cast<char *>(buffer);
    }
}

bool Compiler::initialize() {
    static bool llvm_initialized = false;
    if (!llvm_initialized) {
        if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
            PyErr_SetString(PyExc_SystemError, "cannot initialize llvm");
            return false;
        }
        llvm_initialized = true;
    }

    auto triple = sys::getProcessTriple();
    std::string err;
    auto target = TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        PyErr_SetString(PyExc_SystemError, err.c_str());
        return false;
    }
    machine.reset(target->createTargetMachine(triple, sys::getHostCPUName(), "", {}, Reloc::Model::PIC_));
    if (!machine) {
        PyErr_SetString(PyExc_SystemError, "cannot create TargetMachine");
        return false;
    }
    if (machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile)) {
        PyErr_SetString(PyExc_SystemError, "cannot add passes to emit file");
        return false;
    }

#ifdef ABLATION_BUILD
    if (with_IRO) {
        PassBuilder pb{machine.get()};
        pb.registerModuleAnalyses(opt_MAM);
        pb.registerCGSCCAnalyses(opt_CGAM);
        pb.registerFunctionAnalyses(opt_FAM);
        pb.registerLoopAnalyses(opt_LAM);
        pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
        opt_MPM = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);
    }
#endif
    return true;
}

bool Compiler::compile(Module &mod) {
    assert(!verifyModule(mod, &errs()));
    out_vec.clear();
#ifdef ABLATION_BUILD
    if (with_IRO) {
        opt_MPM.run(mod, opt_MAM);
        out_PM.run(mod);
        opt_MAM.clear();
        opt_CGAM.clear();
        opt_FAM.clear();
        opt_LAM.clear();
    } else {
        out_PM.run(mod);
    }
#else
    out_PM.run(mod);
#endif
    assert(!out_vec.empty());


    constexpr auto set_error = [](auto &expected) {
        PyErr_SetString(PyExc_SystemError, toString(expected.takeError()).c_str());
        return false;
    };

    auto obj = object::ObjectFile::createObjectFile({{out_vec.data(), out_vec.size()}, ""});
    if (!obj) {
        return set_error(obj);
    }

    text_section = {nullptr, 0};
    data_section = {nullptr, 0};
    for (auto &sec : (*obj)->sections()) {
        if (sec.isText()) {
            auto contents = sec.getContents();
            if (!contents) {
                return set_error(contents);
            }
            assert(text_section.empty());
            text_section = *contents;
        } else if (sec.isData()) {
            auto contents = sec.getContents();
            if (!contents) {
                return set_error(contents);
            }
            assert(data_section.empty());
            data_section = *contents;
        }
    }

    assert(!text_section.empty());
    return true;
}

// Is it reasonable to reuse Context, and will this cause garbage accumulation after each compilation?
Context::Context() {
    llvm_context.enableOpaquePointers();
#ifndef DUMP_DEBUG_FILES
    llvm_context.setDiscardValueNames(true);
#endif

    c_null = ConstantPointerNull::get(type<void *>());

    MDBuilder md_builder{llvm_context};
    unlikely = md_builder.createBranchWeights(1, 0xffff);

    auto tbaa_root = md_builder.createTBAARoot("TBAA root for CPython");
    const auto &createTBAA = [&](const char *name, bool is_const = false) {
        // empty name will make tbaa nodes disappear
        assert(name);
        auto scalar_node = md_builder.createTBAANode(name, tbaa_root);
        return md_builder.createTBAAStructTagNode(scalar_node, scalar_node, 0, is_const);
    };
    tbaa_refcnt = createTBAA("reference counter");
    tbaa_obj_field = createTBAA("object field");
    tbaa_frame_field = createTBAA("frame field");
    tbaa_immutable = createTBAA("immutable value", true);
}
