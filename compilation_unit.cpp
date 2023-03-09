#include "compilation_unit.h"

using namespace llvm;


bool CompilationUnit::translate(PyObject *debug_args) {
    llvm_module.setDataLayout(translator.createDataLayout());

    function = Function::Create(translator.type<TargetFunction>(), Function::ExternalLinkage,
            "comPyler_function", &llvm_module);

    DebugInfo debug_info{function, debug_args};

    (runtime_symbols = function->getArg(0))->setName(useName("runtime_symbols"));
    runtime_symbols->addAttr(Attribute::NoAlias);
    runtime_symbols->addAttr(Attribute::ReadOnly);
    (frame_obj = function->getArg(1))->setName(useName("frame"));
    frame_obj->addAttr(Attribute::NoAlias);
    (cframe = function->getArg(2))->setName(useName("cframe"));
    cframe->addAttr(Attribute::NoAlias);
    (eval_breaker = function->getArg(3))->setName(useName("eval_breaker"));
    eval_breaker->addAttr(Attribute::NoAlias);

    parsePyCode();

    auto entry_block = createBlock(function, "entry_block");
    builder.SetInsertPoint(entry_block);
    debug_info.setLocation(builder);

    auto rt_code = loadFieldValue(frame_obj, &PyFrameObject::f_code, translator.tbaa_frame_field);
    rt_names = loadFieldValue(rt_code, &PyCodeObject::co_names, translator.tbaa_immutable);
    rt_consts = loadFieldValue(rt_code, &PyCodeObject::co_consts, translator.tbaa_immutable);

    for (auto &b : PtrRange(blocks, block_num)) {
        static_cast<BasicBlock *>(b)->insertInto(function);
        builder.SetInsertPoint(b);
        stack_height = 0;
        abstract_stack_top = abstract_stack;
        declareStackGrowth(b.initial_stack_height);
        emitBlock(b, debug_info);
        assert(!b.fall_block || stack_height == b.fall().initial_stack_height);
    }
    debug_info.setLocation(builder);

    {
        DynamicArray<llvm::Constant *> handler_pc_arr;
        handler_pc_arr.reserve(handler_num);
        handler_vpc_arr.reserve(handler_num);

        builder.SetInsertPoint(entry_block);
        auto indirect_jump = builder.CreateIndirectBr(builder.CreateInBoundsGEP(
                translator.type<char>(), BlockAddress::get(function, blocks[0]),
                loadFieldValue(cframe, &ExtendedCFrame::handler, translator.tbaa_immutable)
        ), 1 + handler_num);
        indirect_jump->addDestination(blocks[0]);
        unsigned count = 0;
        for (auto &b : PtrRange(blocks, block_num)) {
            if (b.handler_pc) {
                indirect_jump->addDestination(b);
                handler_vpc_arr[count] = b.begin_vpc;
                handler_pc_arr[count] = b.handler_pc;
                count++;
            }
        }
        auto pc_arr_type = ArrayType::get(translator.type<IntPC>(), handler_num);
        new llvm::GlobalVariable(
                llvm_module,
                pc_arr_type,
                true,
                GlobalValue::ExternalLinkage,
                ConstantArray::get(pc_arr_type, ArrayRef(handler_pc_arr + 0, handler_num)),
                "comPyler_handlers"
        );
    }

    if (unbound_error_block) {
        unbound_error_block->insertInto(function);
        builder.SetInsertPoint(unbound_error_block);
        emitCall<raiseUnboundError>();
        builder.CreateUnreachable();
    }

    debug_info.finalize(builder);

    debug_info.dumpModule(llvm_module);
    if (!translator.compile(llvm_module)) {
        return false;
    }
    debug_info.dumpObj(translator.getObjectContent());
    return true;
}

void CompilationUnit::pyIncRef(Value *py_obj) {
#ifdef NON_INLINE_RC
    emitCall<_Py_IncRef>(py_obj);
#else
    Value *ref = py_obj;
    if constexpr (offsetof(PyObject, ob_refcnt)) {
        ref = calcFieldAddr(py_obj, &PyObject::ob_refcnt);
    }
    using RefType = decltype(PyObject::ob_refcnt);
    auto old_value = loadValue<RefType>(ref, translator.tbaa_refcnt);
    auto *delta_1 = getConstantInt<RefType>(1);
    auto new_value = builder.CreateAdd(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, translator.tbaa_refcnt);
#endif
}

void CompilationUnit::pyDecRef(Value *py_obj, bool null_check) {
#ifdef NON_INLINE_RC
    if (!null_check) {
        emitCall<_Py_DecRef>(py_obj);
        return;
    }
#endif
    auto b_end = createBlock(nullptr, "Py_DECREF.end");
    if (null_check) {
        emitUnlikelyJump(builder.CreateICmpEQ(py_obj, translator.c_null), b_end, "decref");
    }
#ifdef NON_INLINE_RC
    emitCall<_Py_DecRef>(py_obj);
#else
    Value *ref = py_obj;
    if constexpr (offsetof(PyObject, ob_refcnt)) {
        ref = calcFieldAddr(py_obj, &PyObject::ob_refcnt);
    }
    using RefType = decltype(PyObject::ob_refcnt);
    auto old_value = loadValue<RefType>(ref, translator.tbaa_refcnt);
    auto new_value = builder.CreateSub(old_value, getConstantInt<RefType>(1));
    storeValue<RefType>(new_value, ref, translator.tbaa_refcnt);

    auto dealloc_block = createBlock(function, "dealloc");
    builder.CreateCondBr(builder.CreateICmpEQ(new_value, getConstantInt<RefType>(0)),
            dealloc_block, b_end, translator.unlikely);
    builder.SetInsertPoint(dealloc_block);

    auto py_type = loadFieldValue(py_obj, &PyObject::ob_type, translator.tbaa_obj_field);
    auto tp_dealloc = loadFieldValue(py_type, &PyTypeObject::tp_dealloc, translator.tbaa_obj_field);
    builder.CreateCall(translator.type<void(PyObject *)>(), tp_dealloc, {py_obj});
#endif
    builder.CreateBr(b_end);
    b_end->insertInto(function);
    builder.SetInsertPoint(b_end);
}

Value *CompilationUnit::getConst(PyOparg oparg) {
    return loadValue<PyObject *>(
            calcElementAddr(rt_consts, offsetof(PyTupleObject, ob_item) + sizeof(PyObject *) * oparg),
            translator.tbaa_immutable,
            useName("const$", oparg, "$", PyTuple_GET_ITEM(py_code->co_consts, oparg)->ob_type->tp_name));
}

std::pair<Value *, Value *> CompilationUnit::getLocal(PyOparg oparg) {
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = calcElementAddr(frame_obj, offset);
    auto value = loadValue<PyObject *>(slot, translator.tbaa_frame_field,
            useName("local$", oparg, "$", PyTuple_GET_ITEM(py_code->co_varnames, oparg)));
    return {slot, value};
}

Value *CompilationUnit::getStackSlot(PyOparg i) {
    assert(stack_height >= i);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (
            stack_height - i +
                    py_code->co_nlocals +
                    PyTuple_GET_SIZE(py_code->co_cellvars) +
                    PyTuple_GET_SIZE(py_code->co_freevars)
    );
    return calcElementAddr(frame_obj, offset);
}

Value *CompilationUnit::fetchStackValue(PyOparg i) {
    auto abs_v = abstract_stack_top[-i];
    if (abs_v.on_stack()) {
        return loadValue<PyObject *>(getStackSlot(stack_height - abs_v.index), translator.tbaa_frame_field,
                useName("stack$", abs_v.index, "$"));
    } else {
        if (abs_v.location == AbstractStackValue::LOCAL) {
            return getLocal(abs_v.index).second;
        } else {
            return getConst(abs_v.index);
        }
    }
}

Value *CompilationUnit::getName(PyOparg i) {
    return loadValue<PyObject *>(
            calcElementAddr(rt_names, offsetof(PyTupleObject, ob_item) + sizeof(PyObject *) * i),
            translator.tbaa_immutable, useName("name$", PyTuple_GET_ITEM(py_code->co_names, i), "$"));
}

Value *CompilationUnit::getFreevar(PyOparg i) {
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (py_code->co_nlocals + i);
    return loadValue<PyObject *>(calcElementAddr(frame_obj, offset), translator.tbaa_frame_field,
            useName("free$", i, "$", i < PyTuple_GET_SIZE(py_code->co_cellvars) ?
                    PyTuple_GET_ITEM(py_code->co_cellvars, i) :
                    PyTuple_GET_ITEM(py_code->co_freevars, i - PyTuple_GET_SIZE(py_code->co_cellvars))
            )
    );
}

Value *CompilationUnit::getSymbol(size_t index) {
    return loadValue<void *>(calcElementAddr<void *>(runtime_symbols, index), translator.tbaa_immutable,
            useName("symbol$", RuntimeSymbols::name_array[index], "$"));
}

void CompilationUnit::emitRotN(PyOparg n) {
    auto abs_top = abstract_stack_top[-1];
    auto top_really_pushed = abs_top.on_stack();
    decltype(n) n_lift = 0;
    for (auto i : IntRange(1, n)) {
        auto &v = abstract_stack_top[-i] = abstract_stack_top[-(i + 1)];
        if (top_really_pushed && v.on_stack()) {
            n_lift++;
            v.index++;
        }
    }
    abstract_stack_top[-n] = abs_top;
    abstract_stack_top[-n].index -= n_lift;

    if (n_lift) {
        if (n_lift <= 8) {
            auto dest = getStackSlot(1);
            auto top = loadValue<PyObject *>(dest, translator.tbaa_frame_field);
            for (auto i : IntRange(n_lift)) {
                auto src = getStackSlot(i + 2);
                auto value = loadValue<PyObject *>(src, translator.tbaa_frame_field);
                storeValue<PyObject *>(value, dest, translator.tbaa_frame_field);
                dest = src;
            }
            storeValue<PyObject *>(top, dest, translator.tbaa_frame_field);
        } else {
            emitCall<handle_ROT_N>(getStackSlot(n_lift + 1), n_lift);
        }
    }
}

void CompilationUnit::declareBlockAsHandler(PyCodeBlock &block) {
    if (!block.handler_pc) {
        auto abs_addr_type = translator.type<uintptr_t>();
        auto to = ConstantExpr::getPointerCast(BlockAddress::get(function, block), abs_addr_type);
        auto from = ConstantExpr::getPointerCast(BlockAddress::get(function, blocks[0]), abs_addr_type);
        block.handler_pc = ConstantExpr::getIntegerCast(ConstantExpr::getSub(to, from),
                translator.type<IntPC>(),
                std::is_signed_v<IntPC>);
        ++handler_num;
    }
}

void CompilationUnit::emitConditionalJump(Value *value, bool cond, BasicBlock *branch, BasicBlock *fall) {
    auto true_block = cond ? branch : fall;
    auto false_block = cond ? fall : branch;

    auto fast_cmp_block = createBlock(function);
    auto slow_cmp_block = createBlock(function);

    auto py_true = getSymbol<_Py_TrueStruct>();
    builder.CreateCondBr(builder.CreateICmpEQ(value, py_true), true_block, fast_cmp_block);
    builder.SetInsertPoint(fast_cmp_block);
    auto py_false = getSymbol<_Py_FalseStruct>();
    builder.CreateCondBr(builder.CreateICmpNE(value, py_false), slow_cmp_block, false_block, translator.unlikely);
    builder.SetInsertPoint(slow_cmp_block);
    builder.CreateCondBr(emitCall<castPyObjectToBool>(value), true_block, false_block);
}

void CompilationUnit::emitCheckEvalBreaker(IntVPC next_vpc) {
    auto next_opcode = _Py_OPCODE(py_code.instrData()[next_vpc]);
    if (next_opcode != SETUP_FINALLY
            && next_opcode != SETUP_WITH
            && next_opcode != BEFORE_ASYNC_WITH
            && next_opcode != YIELD_FROM) {
        return;
    }

    auto eval_breaker_value = builder.Insert(new LoadInst(
            translator.type<int>(),
            eval_breaker,
            useName("eval_breaker_value"),
            false,
            llvm_module.getDataLayout().getABITypeAlign(translator.type<int>()),
            AtomicOrdering::Monotonic
    ));
    auto handle_block = createBlock(function, "eval_breaker.handle");
    emitUnlikelyJump(builder.CreateICmpNE(eval_breaker_value, getConstantInt<int>(0)),
            handle_block, "eval_breaker.end");
    auto end_block = builder.GetInsertBlock();
    builder.SetInsertPoint(handle_block);
    // Note: write PyFrameObject::f_lasti here so that
    // the number of elements in the stack can be correctly known
    // when an error occurs in handleEvalBreaker.
    storeFieldValue(next_vpc, frame_obj, &PyFrameObject::f_lasti, translator.tbaa_frame_field);
    emitCall<handleEvalBreaker>();
    builder.CreateBr(end_block);
    builder.SetInsertPoint(end_block);
}

