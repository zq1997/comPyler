#include "compilation_unit.h"

using namespace llvm;

void CompilationUnit::emitBlock(PyCodeBlock &this_block, DebugInfo &debug_info) {
    auto end_vpc = &this_block < blocks + block_num - 1 ? (&this_block)[1].begin_vpc : py_code.instrNum();
    auto &defined_locals = this_block.locals_input;
    PyOparg extended_oparg = 0;
    IntVPC lasti;
    for (auto vpc : IntRange(this_block.begin_vpc, end_vpc)) {
        debug_info.setLocation(builder, vpc);
        lasti = extended_oparg ? lasti : vpc;
        storeFieldValue(lasti, frame_obj, &PyFrameObject::f_lasti, translator.tbaa_frame_field);
        // Note: Python/compile.c sets MAX_ALLOWED_STACK_USE to 3000
        assert(stack_height >= 0 && stack_height <= UINT_LEAST16_MAX);
        stack_height_arr[vpc] = stack_height;
        auto opcode = _Py_OPCODE(py_code.instrData()[vpc]);
        auto oparg = _Py_OPARG(py_code.instrData()[vpc]) | extended_oparg;
        extended_oparg = 0;

        switch (opcode) {
        case EXTENDED_ARG: {
            extended_oparg = oparg << PyCode::extended_arg_shift;
            assert(extended_oparg);
            break;
        }
        case NOP: {
            break;
        }
        case ROT_TWO: {
            emitRotN(2);
            break;
        }
        case ROT_THREE: {
            emitRotN(3);
            break;
        }
        case ROT_FOUR: {
            emitRotN(4);
            break;
        }
        case ROT_N: {
            emitRotN(oparg);
            break;
        }
        case DUP_TOP: {
            auto top = abstract_stack_top[-1];
            if (top.on_stack()) {
                auto value = fetchStackValue(1);
                pyIncRef(value);
                pyPush(value);
            } else {
                *abstract_stack_top++ = top;
            }
            break;
        }
        case DUP_TOP_TWO: {
            auto second = abstract_stack_top[-2];
            if (second.on_stack()) {
                auto value = fetchStackValue(2);
                pyIncRef(value);
                pyPush(value);
            } else {
                *abstract_stack_top++ = second;
            }
            auto top = abstract_stack_top[-2];
            if (top.on_stack()) {
                auto value = fetchStackValue(2);
                pyIncRef(value);
                pyPush(value);
            } else {
                *abstract_stack_top++ = top;
            }
            break;
        }
        case POP_TOP: {
            auto value = pyPop();
            pyDecRef(value);
            break;
        }
        case LOAD_CONST: {
            auto value = getConst(oparg);
            if (with_SOE && redundant_loads.get(vpc)) {
                *abstract_stack_top++ = {AbstractStackValue::CONST, oparg};
            } else {
                pyPush(value);
                pyIncRef(value);
            }
            break;
        }
        case LOAD_FAST: {
            auto [_, value] = getLocal(oparg);
            if (!with_ICE || !defined_locals.get(oparg)) {
                checkUnboundError(value);
            }
            if (with_SOE && redundant_loads.get(vpc)) {
                *abstract_stack_top++ = {AbstractStackValue::LOCAL, oparg};
            } else {
                pyPush(value);
                pyIncRef(value);
            }
            defined_locals.set(oparg);
            break;
        }
        case STORE_FAST: {
            auto [slot, old_value] = getLocal(oparg);
            auto value = pyPop();
            if (!value.really_pushed) {
                pyIncRef(value);
            }
            storeValue<PyObject *>(value, slot, translator.tbaa_frame_field);
            pyDecRef(old_value, !with_ICE || !defined_locals.get(oparg));
            defined_locals.set(oparg);
            break;
        }
        case DELETE_FAST: {
            auto [slot, old_value] = getLocal(oparg);
            if (!with_ICE || !defined_locals.get(oparg)) {
                checkUnboundError(old_value);
            }
            storeValue<PyObject *>(translator.c_null, slot, translator.tbaa_frame_field);
            pyDecRef(old_value);
            defined_locals.reset(oparg);
            break;
        }
        case LOAD_DEREF: {
            auto cell = getFreevar(oparg);
            auto value = loadFieldValue(cell, &PyCellObject::ob_ref, translator.tbaa_obj_field);
            checkUnboundError(value);
            pyIncRef(value);
            pyPush(value);
            break;
        }
        case LOAD_CLASSDEREF: {
            auto value = emitCall<handle_LOAD_CLASSDEREF>(frame_obj, oparg);
            pyPush(value);
            break;
        }
        case STORE_DEREF: {
            auto cell = getFreevar(oparg);
            auto cell_slot = calcFieldAddr(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, translator.tbaa_obj_field);
            auto value = pyPop();
            if (!value.really_pushed) {
                pyIncRef(value);
            }
            storeValue<PyObject *>(value, cell_slot, translator.tbaa_obj_field);
            pyDecRef(old_value, true);
            break;
        }
        case DELETE_DEREF: {
            auto cell = getFreevar(oparg);
            auto cell_slot = calcFieldAddr(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, translator.tbaa_obj_field);
            checkUnboundError(old_value);
            storeValue<PyObject *>(translator.c_null, cell_slot, translator.tbaa_obj_field);
            pyDecRef(old_value);
            break;
        }
        case LOAD_GLOBAL: {
            auto jit_result = loadFieldValue(cframe, &ExtendedCFrame::translated_result, translator.tbaa_immutable);
            auto opcache_arr = loadFieldValue(jit_result, &TranslatedResult::opcache_arr, translator.tbaa_immutable);
            auto opcache_ptr = calcElementAddr<_PyOpcache>(opcache_arr, opcache_count++);
            auto value = emitCall<handle_LOAD_GLOBAL>(frame_obj, getName(oparg), opcache_ptr);
            pyPush(value);
            break;
        }
        case STORE_GLOBAL: {
            auto value = pyPop();
            emitCall<handle_STORE_GLOBAL>(frame_obj, getName(oparg), value);
            pyDecRef(value);
            break;
        }
        case DELETE_GLOBAL: {
            emitCall<handle_DELETE_GLOBAL>(frame_obj, getName(oparg));
            break;
        }
        case LOAD_NAME: {
            auto value = emitCall<handle_LOAD_NAME>(frame_obj, getName(oparg));
            pyPush(value);
            break;
        }
        case STORE_NAME: {
            auto value = pyPop();
            emitCall<handle_STORE_NAME>(frame_obj, getName(oparg), value);
            pyDecRef(value);
            break;
        }
        case DELETE_NAME: {
            emitCall<handle_DELETE_NAME>(frame_obj, getName(oparg));
            break;
        }
        case LOAD_ATTR: {
            auto owner = pyPop();
            auto jit_result = loadFieldValue(cframe, &ExtendedCFrame::translated_result, translator.tbaa_immutable);
            auto opcache_arr = loadFieldValue(jit_result, &TranslatedResult::opcache_arr, translator.tbaa_immutable);
            auto opcache_ptr = calcElementAddr<_PyOpcache>(opcache_arr, opcache_count++);
            auto attr = emitCall<handle_LOAD_ATTR>(owner, getName(oparg), frame_obj, opcache_ptr);
            pyPush(attr);
            pyDecRef(owner);
            break;
        }
        case LOAD_METHOD: {
            auto stack_top = declareStackShrink(1);
            emitCall<handle_LOAD_METHOD>(getName(oparg), stack_top);
            declareStackGrowth(2);
            break;
        }
        case STORE_ATTR: {
            auto owner = pyPop();
            auto value = pyPop();
            emitCall<handle_STORE_ATTR>(owner, getName(oparg), value);
            pyDecRef(value);
            pyDecRef(owner);
            break;
        }
        case DELETE_ATTR: {
            auto owner = pyPop();
            emitCall<handle_STORE_ATTR>(owner, getName(oparg), translator.c_null);
            pyDecRef(owner);
            break;
        }
        case BINARY_SUBSCR: {
            emitBinaryOperation<handle_BINARY_SUBSCR>();
            break;
        }
        case STORE_SUBSCR: {
            auto sub = pyPop();
            auto container = pyPop();
            auto value = pyPop();
            emitCall<handle_STORE_SUBSCR>(container, sub, value);
            pyDecRef(value);
            pyDecRef(container);
            pyDecRef(sub);
            break;
        }
        case DELETE_SUBSCR: {
            auto sub = pyPop();
            auto container = pyPop();
            emitCall<handle_DELETE_SUBSCR>(container, sub);
            pyDecRef(container);
            pyDecRef(sub);
            break;
        }
        case UNARY_NOT: {
            emitUnaryOperation<handle_UNARY_NOT>();
            break;
        }
        case UNARY_POSITIVE: {
            emitUnaryOperation<handle_UNARY_POSITIVE>();
            break;
        }
        case UNARY_NEGATIVE: {
            emitUnaryOperation<handle_UNARY_NEGATIVE>();
            break;
        }
        case UNARY_INVERT: {
            emitUnaryOperation<handle_UNARY_INVERT>();
            break;
        }
        case BINARY_ADD: {
            emitBinaryOperation<handle_BINARY_ADD>();
            break;
        }
        case INPLACE_ADD: {
            emitBinaryOperation<handle_INPLACE_ADD>();
            break;
        }
        case BINARY_SUBTRACT: {
            emitBinaryOperation<handle_BINARY_SUBTRACT>();
            break;
        }
        case INPLACE_SUBTRACT: {
            emitBinaryOperation<handle_INPLACE_SUBTRACT>();
            break;
        }
        case BINARY_MULTIPLY: {
            emitBinaryOperation<handle_BINARY_MULTIPLY>();
            break;
        }
        case INPLACE_MULTIPLY: {
            emitBinaryOperation<handle_INPLACE_MULTIPLY>();
            break;
        }
        case BINARY_FLOOR_DIVIDE: {
            emitBinaryOperation<handle_BINARY_FLOOR_DIVIDE>();
            break;
        }
        case INPLACE_FLOOR_DIVIDE: {
            emitBinaryOperation<handle_INPLACE_FLOOR_DIVIDE>();
            break;
        }
        case BINARY_TRUE_DIVIDE: {
            emitBinaryOperation<handle_BINARY_TRUE_DIVIDE>();
            break;
        }
        case INPLACE_TRUE_DIVIDE: {
            emitBinaryOperation<handle_INPLACE_TRUE_DIVIDE>();
            break;
        }
        case BINARY_MODULO: {
            emitBinaryOperation<handle_BINARY_MODULO>();
            break;
        }
        case INPLACE_MODULO: {
            emitBinaryOperation<handle_INPLACE_MODULO>();
            break;
        }
        case BINARY_POWER: {
            emitBinaryOperation<handle_BINARY_POWER>();
            break;
        }
        case INPLACE_POWER: {
            emitBinaryOperation<handle_INPLACE_POWER>();
            break;
        }
        case BINARY_MATRIX_MULTIPLY: {
            emitBinaryOperation<handle_BINARY_MATRIX_MULTIPLY>();
            break;
        }
        case INPLACE_MATRIX_MULTIPLY: {
            emitBinaryOperation<handle_INPLACE_MATRIX_MULTIPLY>();
            break;
        }
        case BINARY_LSHIFT: {
            emitBinaryOperation<handle_BINARY_LSHIFT>();
            break;
        }
        case INPLACE_LSHIFT: {
            emitBinaryOperation<handle_INPLACE_LSHIFT>();
            break;
        }
        case BINARY_RSHIFT: {
            emitBinaryOperation<handle_BINARY_RSHIFT>();
            break;
        }
        case INPLACE_RSHIFT: {
            emitBinaryOperation<handle_INPLACE_RSHIFT>();
            break;
        }
        case BINARY_AND: {
            emitBinaryOperation<handle_BINARY_AND>();
            break;
        }
        case INPLACE_AND: {
            emitBinaryOperation<handle_INPLACE_AND>();
            break;
        }
        case BINARY_OR: {
            emitBinaryOperation<handle_BINARY_OR>();
            break;
        }
        case INPLACE_OR: {
            emitBinaryOperation<handle_INPLACE_OR>();
            break;
        }
        case BINARY_XOR: {
            emitBinaryOperation<handle_BINARY_XOR>();
            break;
        }
        case INPLACE_XOR: {
            emitBinaryOperation<handle_INPLACE_XOR>();
            break;
        }
        case COMPARE_OP: {
            auto right = pyPop();
            auto left = pyPop();
            auto res = emitCall<handle_COMPARE_OP>(left, right, oparg);
            pyPush(res);
            pyDecRef(left);
            pyDecRef(right);
            break;
        }
        case IS_OP: {
            auto right = pyPop();
            auto left = pyPop();
            auto res = builder.CreateICmpEQ(left, right);
            auto py_true = getSymbol<_Py_TrueStruct>();
            auto py_false = getSymbol<_Py_FalseStruct>();
            auto value_for_true = !oparg ? py_true : py_false;
            auto value_for_false = !oparg ? py_false : py_true;
            auto value = builder.CreateSelect(res, value_for_true, value_for_false);
            pyIncRef(value);
            pyPush(value);
            pyDecRef(left);
            pyDecRef(right);
            break;
        }
        case CONTAINS_OP: {
            auto right = pyPop();
            auto left = pyPop();
            auto value = emitCall<handle_CONTAINS_OP>(left, right, oparg);
            pyPush(value);
            pyDecRef(left);
            pyDecRef(right);
            break;
        }
        case RETURN_VALUE: {
            auto retval = pyPop();
            assert(retval.really_pushed);
            assert(stack_height == 0);
            returnFrame(FRAME_RETURNED, retval);
            return;
        }
        case CALL_FUNCTION: {
            auto func_args = declareStackShrink(oparg + 1);
            auto ret = emitCall<handle_CALL_FUNCTION>(func_args, oparg);
            pyPush(ret);
            emitCheckEvalBreaker(vpc + 1);
            break;
        }
        case CALL_METHOD: {
            auto func_args = declareStackShrink(oparg + 2);
            auto ret = emitCall<handle_CALL_METHOD>(func_args, oparg);
            pyPush(ret);
            emitCheckEvalBreaker(vpc + 1);
            break;
        }
        case CALL_FUNCTION_KW: {
            auto kwnames = pyPop();
            auto func_args = declareStackShrink(oparg + 1);
            auto ret = emitCall<handle_CALL_FUNCTION_KW>(func_args, oparg, kwnames);
            pyDecRef(kwnames);
            pyPush(ret);
            emitCheckEvalBreaker(vpc + 1);
            break;
        }
        case CALL_FUNCTION_EX: {
            Value *kwargs = translator.c_null;
            bool really_pushed = false;
            if (oparg & 1) {
                auto poped_step = pyPop();
                kwargs = poped_step;
                really_pushed = poped_step.really_pushed;
            }
            auto args = pyPop();
            auto callable = pyPop();
            auto ret = emitCall<handle_CALL_FUNCTION_EX>(callable, args, kwargs);
            pyPush(ret);
            if (really_pushed) {
                pyDecRef(kwargs);
            }
            pyDecRef(args);
            pyDecRef(callable);
            emitCheckEvalBreaker(vpc + 1);
            break;
        }
        case LOAD_CLOSURE: {
            auto cell = getFreevar(oparg);
            pyIncRef(cell);
            pyPush(cell);
            break;
        }
        case MAKE_FUNCTION: {
            auto qualname = pyPop();
            auto codeobj = pyPop();
            auto extra = getStackSlot();
            auto py_func = emitCall<handle_MAKE_FUNCTION>(codeobj, frame_obj, qualname, extra, oparg);
            declareStackShrink(!!(oparg & 1) + !!(oparg & 2) + !!(oparg & 4) + !!(oparg & 8));
            pyPush(py_func);
            pyDecRef(codeobj);
            pyDecRef(qualname);
            break;
        }
        case LOAD_BUILD_CLASS: {
            auto bc = emitCall<handle_LOAD_BUILD_CLASS>(frame_obj);
            pyPush(bc);
            break;
        }

        case IMPORT_NAME: {
            auto name = getName(oparg);
            auto fromlist = pyPop();
            auto level = pyPop();
            auto res = emitCall<handle_IMPORT_NAME>(frame_obj, name, fromlist, level);
            pyPush(res);
            pyDecRef(level);
            pyDecRef(fromlist);
            break;
        }
        case IMPORT_FROM: {
            auto from = fetchStackValue(1);
            auto res = emitCall<handle_IMPORT_FROM>(from, getName(oparg));
            pyPush(res);
            break;
        }
        case IMPORT_STAR: {
            auto from = pyPop();
            emitCall<handle_IMPORT_STAR>(frame_obj, from);
            pyDecRef(from);
            break;
        }

        case JUMP_FORWARD: {
            builder.CreateBr(this_block.branch());
            return;
        }
        case JUMP_ABSOLUTE: {
            if (this_block.branch().begin_vpc <= this_block.begin_vpc) {
                declareBlockAsHandler(this_block.branch());
            }

            emitCheckEvalBreaker(this_block.branch().begin_vpc);
            builder.CreateBr(this_block.branch());
            return;
        }
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE: {
            if (this_block.branch().begin_vpc <= this_block.begin_vpc) {
                declareBlockAsHandler(this_block.branch());
            }
            auto cond_obj = pyPop();
            auto pre_branch = createBlock(function);
            auto pre_fall = cond_obj.really_pushed ? createBlock(function) : this_block.fall();
            emitConditionalJump(cond_obj, opcode == POP_JUMP_IF_TRUE, pre_branch, pre_fall);
            if (cond_obj.really_pushed) {
                builder.SetInsertPoint(pre_fall);
                pyDecRef(cond_obj);
                builder.CreateBr(this_block.fall());
            }
            builder.SetInsertPoint(pre_branch);
            pyDecRef(cond_obj);
            emitCheckEvalBreaker(this_block.branch().begin_vpc);
            builder.CreateBr(this_block.branch());
            return;
        }
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP: {
            auto value = pyPop();
            assert(value.really_pushed);
            auto pre_fall = createBlock(function);
            emitConditionalJump(value, opcode == JUMP_IF_TRUE_OR_POP, this_block.branch(), pre_fall);
            builder.SetInsertPoint(pre_fall);
            pyDecRef(value);
            builder.CreateBr(this_block.fall());
            return;
        }
        case GET_ITER: {
            auto iterable = pyPop();
            auto iter = emitCall<handle_GET_ITER>(iterable);
            pyDecRef(iterable);
            pyPush(iter);
            break;
        }
        case FOR_ITER: {
            assert(abstract_stack_top[-1].on_stack());
            auto iter = fetchStackValue(1);
            auto the_type = loadFieldValue(iter, &PyObject::ob_type, translator.tbaa_obj_field);
            auto the_iternextfunc = loadFieldValue(the_type, &PyTypeObject::tp_iternext, translator.tbaa_obj_field);
            auto next = builder.CreateCall(translator.type<std::remove_pointer_t<iternextfunc>>(),
                    the_iternextfunc, {iter});
            pyPush(next);
            auto break_block = createBlock(function, "FOR_ITER.break");
            builder.CreateCondBr(builder.CreateICmpEQ(next, translator.c_null),
                    break_block, this_block.fall(), translator.unlikely);
            builder.SetInsertPoint(break_block);
            emitCall<handle_FOR_ITER>(iter);
            builder.CreateBr(this_block.branch());
            return;
        }

        case BUILD_STRING: {
            auto values = declareStackShrink(oparg);
            auto str = emitCall<handle_BUILD_STRING>(values, oparg);
            pyPush(str);
            break;
        }
        case BUILD_TUPLE: {
            auto values = declareStackShrink(oparg);
            auto map = emitCall<handle_BUILD_TUPLE>(values, oparg);
            pyPush(map);
            break;
        }
        case BUILD_LIST: {
            auto values = declareStackShrink(oparg);
            auto map = emitCall<handle_BUILD_LIST>(values, oparg);
            pyPush(map);
            break;
        }
        case BUILD_SET: {
            auto values = declareStackShrink(oparg);
            auto map = emitCall<handle_BUILD_SET>(values, oparg);
            pyPush(map);
            break;
        }
        case BUILD_MAP: {
            auto values = declareStackShrink(2 * oparg);
            auto map = emitCall<handle_BUILD_MAP>(values, oparg);
            pyPush(map);
            break;
        }
        case BUILD_CONST_KEY_MAP: {
            auto values = declareStackShrink(oparg + 1);
            auto map = emitCall<handle_BUILD_CONST_KEY_MAP>(values, oparg);
            pyPush(map);
            break;
        }
        case LIST_APPEND: {
            auto value = pyPop();
            auto list = fetchStackValue(oparg);
            emitCall<handle_LIST_APPEND>(list, value);
            pyDecRef(value);
            break;
        }
        case SET_ADD: {
            auto value = pyPop();
            auto set = fetchStackValue(oparg);
            emitCall<handle_SET_ADD>(set, value);
            pyDecRef(value);
            break;
        }
        case MAP_ADD: {
            auto value = pyPop();
            auto key = pyPop();
            auto map = fetchStackValue(oparg);
            emitCall<handle_MAP_ADD>(map, key, value);
            pyDecRef(key);
            pyDecRef(value);
            break;
        }
        case LIST_EXTEND: {
            auto iterable = pyPop();
            auto list = fetchStackValue(oparg);
            emitCall<handle_LIST_EXTEND>(list, iterable);
            pyDecRef(iterable);
            break;
        }
        case SET_UPDATE: {
            auto iterable = pyPop();
            auto set = fetchStackValue(oparg);
            emitCall<handle_SET_UPDATE>(set, iterable);
            pyDecRef(iterable);
            break;
        }
        case DICT_UPDATE: {
            auto update = pyPop();
            auto dict = fetchStackValue(oparg);
            emitCall<handle_DICT_UPDATE>(dict, update);
            pyDecRef(update);
            break;
        }
        case DICT_MERGE: {
            auto update = pyPop();
            auto dict = fetchStackValue(oparg);
            auto callee = fetchStackValue(oparg + 2);
            emitCall<handle_DICT_MERGE>(callee, dict, update);
            pyDecRef(update);
            break;
        }
        case LIST_TO_TUPLE: {
            auto list = pyPop();
            auto tuple = emitCall<handle_LIST_TO_TUPLE>(list);
            pyPush(tuple);
            pyDecRef(list);
            break;
        }

        case FORMAT_VALUE: {
            Value *fmt_spec = translator.c_null;
            bool really_pushed = false;
            if ((oparg & FVS_MASK) == FVS_HAVE_SPEC) {
                auto poped_fmt_spec = pyPop();
                fmt_spec = poped_fmt_spec;
                really_pushed = poped_fmt_spec.really_pushed;
            }
            auto value = pyPop();
            int which_conversion = oparg & FVC_MASK;
            auto result = emitCall<handle_FORMAT_VALUE>(value, fmt_spec, which_conversion);
            pyPush(result);
            pyDecRef(value);
            if (really_pushed) {
                pyDecRef(fmt_spec);
            }
            break;
        }
        case BUILD_SLICE: {
            Value *step = translator.c_null;
            bool really_pushed = false;
            if (oparg == 3) {
                auto poped_step = pyPop();
                step = poped_step;
                really_pushed = poped_step.really_pushed;
            }
            auto stop = pyPop();
            auto start = pyPop();
            auto slice = emitCall<handle_BUILD_SLICE>(start, stop, step);
            pyPush(slice);
            pyDecRef(start);
            pyDecRef(stop);
            if (really_pushed) {
                pyDecRef(step);
            }
            break;
        }
        case LOAD_ASSERTION_ERROR: {
            auto ptr = getSymbol<PyExc_AssertionError>();
            auto value = loadValue<void *>(ptr, translator.tbaa_immutable);
            pyIncRef(value);
            pyPush(value);
            break;
        }
        case SETUP_ANNOTATIONS: {
            emitCall<handle_SETUP_ANNOTATIONS>(frame_obj);
            break;
        }
        case PRINT_EXPR: {
            auto value = pyPop();
            emitCall<handle_PRINT_EXPR>(value);
            pyDecRef(value);
            break;
        }

        case UNPACK_SEQUENCE: {
            auto seq = pyPop();
            emitCall<handle_UNPACK_SEQUENCE>(seq, oparg, getStackSlot());
            pyDecRef(seq);
            declareStackGrowth(oparg);
            break;
        }
        case UNPACK_EX: {
            Py_ssize_t before_star = oparg & 0xFF;
            Py_ssize_t after_star = oparg >> 8;
            auto seq = pyPop();
            emitCall<handle_UNPACK_EX>(seq, before_star, after_star, getStackSlot());
            pyDecRef(seq);
            declareStackGrowth(before_star + 1 + after_star);
            break;
        }

        case GET_LEN: {
            auto value = fetchStackValue(1);
            auto len = emitCall<hanlde_GET_LEN>(value);
            pyPush(len);
            break;
        }
        case MATCH_MAPPING:
        case MATCH_SEQUENCE: {
            auto test_flag = opcode == MATCH_SEQUENCE ? Py_TPFLAGS_SEQUENCE : Py_TPFLAGS_MAPPING;
            auto subject = fetchStackValue(1);
            auto ob_type = loadFieldValue(subject, &PyObject::ob_type, translator.tbaa_obj_field);
            auto tp_flags = loadFieldValue(ob_type, &PyTypeObject::tp_flags, translator.tbaa_obj_field);
            auto match = builder.CreateAnd(tp_flags, getConstantInt<decltype(PyTypeObject::tp_flags)>(test_flag));
            auto match_bool = builder.CreateICmpNE(match, getConstantInt<decltype(PyTypeObject::tp_flags)>(0));
            auto py_true = getSymbol<_Py_TrueStruct>();
            auto py_false = getSymbol<_Py_FalseStruct>();
            auto res = builder.CreateSelect(match_bool, py_true, py_false);
            pyIncRef(res);
            pyPush(res);
            break;
        }
        case MATCH_KEYS: {
            emitCall<hanlde_MATCH_KEYS>(getStackSlot());
            declareStackGrowth(2);
            break;
        }
        case MATCH_CLASS: {
            auto kwargs = pyPop();
            auto inputs_outputs = declareStackShrink(2);
            emitCall<hanlde_MATCH_CLASS>(oparg, kwargs, inputs_outputs);
            declareStackGrowth(2);
            pyDecRef(kwargs);
            break;
        }
        case COPY_DICT_WITHOUT_KEYS: {
            auto keys = pyPop();
            auto subject = fetchStackValue(1);
            auto rest = emitCall<handle_COPY_DICT_WITHOUT_KEYS>(subject, keys);
            pyPush(rest);
            pyDecRef(keys);
            break;
        }

        case SETUP_FINALLY: {
            declareBlockAsHandler(this_block.branch());
            emitCall<PyFrame_BlockSetup>(frame_obj, SETUP_FINALLY, this_block.branch().begin_vpc, stack_height);
            builder.CreateBr(this_block.fall());
            return;
        }
        case POP_BLOCK: {
            emitCall<PyFrame_BlockPop>(frame_obj);
            break;
        }
        case POP_EXCEPT: {
            auto stack_top = declareStackShrink(3);
            emitCall<handle_POP_EXCEPT>(frame_obj, stack_top);
            break;
        }
        case JUMP_IF_NOT_EXC_MATCH: {
            auto right = pyPop();
            auto left = pyPop();
            auto match = emitCall<handle_JUMP_IF_NOT_EXC_MATCH>(left, right);
            pyDecRef(left);
            pyDecRef(right);
            builder.CreateCondBr(match, this_block.fall(), this_block.branch());
            return;
        }
        case RERAISE: {
            emitCall<handle_RERAISE>(oparg, stack_height);
            builder.CreateUnreachable();
            return;
        }
        case SETUP_WITH: {
            declareBlockAsHandler(this_block.branch());
            emitCall<handle_SETUP_WITH>(frame_obj, this_block.branch().begin_vpc, stack_height);
            declareStackGrowth(1);
            builder.CreateBr(this_block.fall());
            return;
        }
        case WITH_EXCEPT_START: {
            auto res = emitCall<handle_WITH_EXCEPT_START>(getStackSlot());
            pyPush(res);
            break;
        }
        case RAISE_VARARGS: {
            assert(0 <= oparg && oparg <= 2);
            emitCall<handle_RAISE_VARARGS>(oparg, stack_height);
            builder.CreateUnreachable();
            return;
        }

        case GEN_START: {
            auto should_be_none = pyPop();
            pyDecRef(should_be_none);
            break;
        }
        case YIELD_VALUE: {
            auto retval = pyPop();
            assert(retval.really_pushed);
            Value *retval_ = retval;
            if (py_code->co_flags & CO_ASYNC_GENERATOR) {
                retval_ = emitCall<handle_YIELD_VALUE>(retval_);
            }
            returnFrame(FRAME_SUSPENDED, retval_);
            declareStackGrowth(1);
            declareBlockAsHandler(this_block.fall());
            return;
        }
        case YIELD_FROM: {
            auto inputs_outputs = declareStackShrink(2);
            auto value = emitCall<handle_YIELD_FROM>(inputs_outputs);
            declareStackGrowth(1);

            auto b_finish = createBlock(nullptr, "YIELD_FROM.finish");
            emitUnlikelyJump(builder.CreateICmpEQ(value, translator.c_null), b_finish, "YIELD_FROM.next");

            declareBlockAsHandler(this_block);
            storeFieldValue(lasti - 1, frame_obj, &PyFrameObject::f_lasti, translator.tbaa_frame_field);
            returnFrame(FRAME_SUSPENDED, value);

            b_finish->insertInto(function);
            builder.SetInsertPoint(b_finish);
            break;
        }
        case GET_YIELD_FROM_ITER: {
            auto iterable = pyPop();
            bool is_coroutine = py_code->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE);
            auto iter = emitCall<handle_GET_YIELD_FROM_ITER>(iterable, is_coroutine);
            pyPush(iter);
            pyDecRef(iterable);
            break;
        }
        case GET_AWAITABLE: {
            auto iterable = pyPop();
            auto prev_op = _Py_OPCODE(py_code.instrData()[vpc - 1]);
            auto prev_prev_op = vpc >= 2 ? _Py_OPCODE(py_code.instrData()[vpc - 2]) : 0;
            auto iter = emitCall<handle_GET_AWAITABLE>(iterable, prev_prev_op, prev_op);
            pyPush(iter);
            pyDecRef(iterable);
            break;
        }
        case GET_AITER: {
            auto obj = pyPop();
            auto iter = emitCall<handle_GET_AITER>(obj);
            pyPush(iter);
            pyDecRef(obj);
            break;
        }
        case GET_ANEXT: {
            auto aiter = fetchStackValue(1);
            auto awaitable = emitCall<handle_GET_ANEXT>(aiter);
            pyPush(awaitable);
            break;
        }
        case END_ASYNC_FOR: {
            emitCall<handle_END_ASYNC_FOR>(stack_height);
            declareStackShrink(7);
            break;
        }
        case SETUP_ASYNC_WITH: {
            assert(abstract_stack_top[-1].on_stack());
            declareBlockAsHandler(this_block.branch());
            emitCall<PyFrame_BlockSetup>(frame_obj, SETUP_FINALLY, this_block.branch().begin_vpc, stack_height - 1);
            builder.CreateBr(this_block.fall());
            return;
        }
        case BEFORE_ASYNC_WITH: {
            emitCall<handle_BEFORE_ASYNC_WITH>(getStackSlot());
            declareStackGrowth(1);
            break;
        }
        default:
            Py_UNREACHABLE();
        }
    }

    assert(this_block.fall_block && !this_block.branch_block);
    assert(!builder.GetInsertBlock()->getTerminator());
    builder.CreateBr(this_block.fall());
}
