#ifndef COMPYLER_DEBUG_INFO_H
#define COMPYLER_DEBUG_INFO_H

#include <Python.h>
#include <frameobject.h>
#include <opcode.h>

#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>

class DebugInfoImpl {
    llvm::DIBuilder di_builder;
    PyObject *debug_args;
    llvm::DISubprogram *di_func;

    const char *get_debug_arg(int n) { return PyBytes_AS_STRING(PyTuple_GET_ITEM(debug_args, n)); }

    void dumpDebugFile(int n, const std::function<void(llvm::raw_fd_ostream &)> &do_writing) {
        if (debug_args) {
            auto path = get_debug_arg(n);
            std::error_code ec;
            llvm::raw_fd_ostream fd_os(path, ec);
            if (!ec) {
                do_writing(fd_os);
            }
            if (ec || fd_os.has_error()) {
                fprintf(stderr, "cannot write debug file: %s", path);
            }
        }
    }

public:
    explicit DebugInfoImpl(llvm::Function *function, PyObject *debug_args) :
            di_builder{*function->getParent(), false}, debug_args{debug_args} {
        if (!debug_args) {
            return;
        }
        auto di_file = di_builder.createFile(get_debug_arg(1), "");
        di_builder.createCompileUnit(llvm::dwarf::DW_LANG_C, di_file, "", false, "", 0);
        di_func = di_builder.createFunction(di_file, "", "", di_file,
                1, di_builder.createSubroutineType({}), 1,
                llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
        function->setSubprogram(di_func);
    }

    void setLocation(IRBuilder &ir_builder, int vpc = -1) {
        if (debug_args) {
            ir_builder.getInserter().setDebugLocation(
                    llvm::DILocation::get(ir_builder.getContext(), vpc + 2, 0, di_func));
        }
    }

    void finalize(IRBuilder &ir_builder) {
        if (debug_args) {
            di_builder.finalize();
        }
    }

    void dumpObj(llvm::SmallVector<char> &obj) {
        dumpDebugFile(3, [&](llvm::raw_fd_ostream &os) { os.write(obj.data(), obj.size()); });
    }

    void dumpModule(llvm::Module &mod) {
        dumpDebugFile(2, [&](llvm::raw_fd_ostream &os) { mod.print(os, nullptr); });
    }
};

class NullDebugInfoImpl {
public:
    explicit NullDebugInfoImpl(const auto &...) {}

    void setLocation(const auto &...) {}

    void finalize(const auto &...) {}

    void dumpObj(const auto &...) {}

    void dumpModule(const auto &...) {}
};

#ifdef DUMP_DEBUG_FILES
using DebugInfo = DebugInfoImpl;
#else
using DebugInfo = NullDebugInfoImpl;
#endif

#endif
