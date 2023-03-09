#include "compilation_unit.h"

constexpr bool isTerminator(int opcode) {
    switch (opcode) {
    case RETURN_VALUE:
    case RERAISE:
    case RAISE_VARARGS:
    case JUMP_ABSOLUTE:
    case JUMP_FORWARD:
        return true;
    default:
        return false;
    }
}

constexpr bool isAbsoluteJmp(int opcode) {
    switch (opcode) {
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_FALSE:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_ABSOLUTE:
        return true;
    default:
        return false;
    }
}

constexpr bool isRelativeJump(int opcode) {
    switch (opcode) {
    case FOR_ITER:
    case JUMP_FORWARD:
        return true;
    default:
        return false;
    }
}

constexpr bool isTryBlockSetup(int opcode) {
    switch (opcode) {
    case SETUP_FINALLY:
    case SETUP_WITH:
    case SETUP_ASYNC_WITH:
        return true;
    default:
        return false;
    }
}

class ReversedAbstractStack {
    static constexpr auto must_be_pushed = std::numeric_limits<IntVPC>::max();

    const ssize_t size;
    IntVPC *stack;
    IntVPC *sp;
    IntVPC current_timestamp;
    bool new_space;
public:
    template <typename U>
    explicit ReversedAbstractStack(U *reusable_space, ssize_t size) : size(size) {
        if constexpr (sizeof(U) < sizeof(IntVPC) * 2) {
            stack = new IntVPC[size * 2];
            new_space = true;
        } else {
            stack = reinterpret_cast<IntVPC *>(reusable_space);
            new_space = false;
        }
    }

    ~ReversedAbstractStack() {
        if (new_space) {
            delete[] stack;
        }
    }

    void reset() {
        for (auto &underflow : PtrRange(stack, size)) {
            underflow = must_be_pushed;
        }
        sp = stack + size;
    }

    void setTimestamp(IntVPC t) { current_timestamp = t; }

    int height() { return (stack + size) - sp; }

    void fetch(int offset_from_top) {
        assert(sp[-offset_from_top] > current_timestamp);
    }

    void pop() {
        *sp++ = current_timestamp;
    }

    void popFromStack() {
        *sp++ = must_be_pushed;
    }

    void popConsecutivelyFromStack(int n) {
        while (n--) {
            popFromStack();
        }
    }

    auto push() {
        return *--sp;
    }

    void rot_n(int n) {
        auto top = sp[-n];
        while (--n) {
            sp[-(n + 1)] = sp[-n];
        }
        sp[-1] = top;
    }

    template <int N>
    void dup_n() {
        sp -= N;
        for (auto i : IntRange(N)) {
            sp[i - N] = sp[i - N] > sp[i] ? sp[i - N] : sp[i];
        }
    }
};

static void performIntraBlockAnalysis(PyCodeBlock &block, IntVPC end_vpc,
        PyCode py_code, ReversedAbstractStack &stack,
        DynamicArray<IntVPC> &timestamp_for_locals, BitArray &redundant_loads) {
    auto first_instr = py_code.instrData();
    for (auto vpc = end_vpc; vpc > block.begin_vpc;) {
        --vpc;
        auto opcode = _Py_OPCODE(first_instr[vpc]);
        auto oparg = _Py_OPARG(first_instr[vpc]);
        for (unsigned shift = 0; vpc && _Py_OPCODE(first_instr[vpc - 1]) == EXTENDED_ARG;) {
            shift += PyCode::extended_arg_shift;
            oparg |= _Py_OPARG(first_instr[--vpc]) << shift;
        }
        stack.setTimestamp(vpc);

        switch (opcode) {
        case NOP:
            break;
        case ROT_TWO:
            stack.rot_n(2);
            break;
        case ROT_THREE: {
            stack.rot_n(3);
            break;
        }
        case ROT_FOUR: {
            stack.rot_n(4);
            break;
        }
        case ROT_N:
            stack.rot_n(oparg);
            break;
        case DUP_TOP:
            stack.dup_n<1>();
            break;
        case DUP_TOP_TWO:
            stack.dup_n<2>();
            break;
        case POP_TOP:
            stack.pop();
            break;

        case LOAD_CONST: {
            auto t = stack.push();
            redundant_loads.setIf(vpc, t < end_vpc);
            break;
        }
        case LOAD_FAST: {
            auto t = stack.push();
            redundant_loads.setIf(vpc, t < timestamp_for_locals[oparg]);
            auto is_the_last_touch = block.locals_touched.testAndSet(oparg);
            block.locals_set.setIf(oparg, is_the_last_touch);
            break;
        }
        case STORE_FAST: {
            stack.pop();
            timestamp_for_locals[oparg] = vpc;
            auto is_the_last_touch = block.locals_touched.testAndSet(oparg);
            block.locals_set.setIf(oparg, is_the_last_touch);
            break;
        }
        case DELETE_FAST: {
            timestamp_for_locals[oparg] = vpc;
            block.locals_touched.set(oparg);
            block.getBitArrayForLocalsEverDeleted().set(oparg);
            break;
        }
        case LOAD_DEREF:
        case LOAD_CLASSDEREF:
            stack.push();
            break;
        case STORE_DEREF:
            stack.pop();
            break;
        case DELETE_DEREF:
            break;
        case LOAD_GLOBAL:
            stack.push();
            break;
        case STORE_GLOBAL:
            stack.pop();
            break;
        case DELETE_GLOBAL:
            break;
        case LOAD_NAME:
            stack.push();
            break;
        case STORE_NAME:
            stack.pop();
            break;
        case DELETE_NAME:
            break;

        case LOAD_ATTR:
            stack.push();
            stack.pop();
            break;
        case LOAD_METHOD:
            stack.push();
            stack.push();
            stack.popFromStack();
            break;
        case STORE_ATTR:
            stack.pop();
            stack.pop();
            break;
        case DELETE_ATTR:
            stack.pop();
            break;
        case BINARY_SUBSCR:
            stack.push();
            stack.pop();
            stack.pop();
            break;
        case STORE_SUBSCR:
            stack.pop();
            stack.pop();
            stack.pop();
            break;
        case DELETE_SUBSCR:
            stack.pop();
            stack.pop();
            break;

        case UNARY_NOT:
        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_INVERT:
            stack.push();
            stack.pop();
            break;
        case BINARY_ADD:
        case INPLACE_ADD:
        case BINARY_SUBTRACT:
        case INPLACE_SUBTRACT:
        case BINARY_MULTIPLY:
        case INPLACE_MULTIPLY:
        case BINARY_FLOOR_DIVIDE:
        case INPLACE_FLOOR_DIVIDE:
        case BINARY_TRUE_DIVIDE:
        case INPLACE_TRUE_DIVIDE:
        case BINARY_MODULO:
        case INPLACE_MODULO:
        case BINARY_POWER:
        case INPLACE_POWER:
        case BINARY_MATRIX_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case BINARY_LSHIFT:
        case INPLACE_LSHIFT:
        case BINARY_RSHIFT:
        case INPLACE_RSHIFT:
        case BINARY_AND:
        case INPLACE_AND:
        case BINARY_OR:
        case INPLACE_OR:
        case BINARY_XOR:
        case INPLACE_XOR:
        case COMPARE_OP:
        case IS_OP:
        case CONTAINS_OP:
            stack.push();
            stack.pop();
            stack.pop();
            break;

        case RETURN_VALUE:
            // Make sure it's on the stack, so that its reference is borrowed after popping.
            stack.popFromStack();
            break;

        case CALL_FUNCTION:
            stack.push();
            stack.popConsecutivelyFromStack(1 + oparg);
            break;
        case CALL_METHOD:
            stack.push();
            stack.popConsecutivelyFromStack(2 + oparg);
            break;
        case CALL_FUNCTION_KW:
            stack.push();
            stack.popConsecutivelyFromStack(1 + oparg);
            stack.pop();
            break;
        case CALL_FUNCTION_EX:
            stack.push();
            stack.pop();
            stack.pop();
            if (oparg & 1) {
                stack.pop();
            }
            break;
        case LOAD_CLOSURE:
            stack.push();
            break;
        case MAKE_FUNCTION: {
            stack.push();
            stack.popConsecutivelyFromStack(!!(oparg & 1) + !!(oparg & 2) + !!(oparg & 4) + !!(oparg & 8));
            stack.pop();
            stack.pop();
            break;
        }
        case LOAD_BUILD_CLASS:
            stack.push();
            break;

        case IMPORT_NAME:
            stack.push();
            stack.pop();
            stack.pop();
            break;
        case IMPORT_FROM:
            stack.push();
            stack.fetch(1);
            break;
        case IMPORT_STAR:
            stack.pop();
            break;

        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
            block.branch_stack_difference = 0;
            break;
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
            block.branch_stack_difference = 0;
            stack.pop();
            break;
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            block.branch_stack_difference = 1;
            stack.popFromStack();
            break;
        case GET_ITER:
            stack.push();
            stack.pop();
            break;
        case FOR_ITER:
            block.branch_stack_difference = -2;
            stack.push();
            stack.fetch(1);
            break;

        case BUILD_STRING:
        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_SET:
            stack.push();
            stack.popConsecutivelyFromStack(oparg);
            break;
        case BUILD_MAP:
            stack.push();
            stack.popConsecutivelyFromStack(2 * oparg);
            break;
        case BUILD_CONST_KEY_MAP:
            stack.push();
            stack.popConsecutivelyFromStack(oparg + 1);
            break;
        case LIST_APPEND:
        case SET_ADD:
            stack.fetch(oparg);
            stack.pop();
            break;
        case MAP_ADD:
            stack.fetch(oparg);
            stack.pop();
            stack.pop();
            break;
        case LIST_EXTEND:
        case SET_UPDATE:
        case DICT_UPDATE:
            stack.fetch(oparg);
            stack.pop();
            break;
        case DICT_MERGE: {
            stack.fetch(oparg + 2);
            stack.fetch(oparg);
            stack.pop();
            break;
        }
        case LIST_TO_TUPLE:
            stack.push();
            stack.pop();
            break;

        case FORMAT_VALUE:
            stack.push();
            stack.pop();
            if ((oparg & FVS_MASK) == FVS_HAVE_SPEC) {
                stack.pop();
            }
            break;
        case BUILD_SLICE:
            stack.push();
            stack.pop();
            stack.pop();
            if (oparg == 3) {
                stack.pop();
            }
            break;
        case LOAD_ASSERTION_ERROR:
            stack.push();
            break;
        case SETUP_ANNOTATIONS:
            break;
        case PRINT_EXPR:
            stack.pop();
            break;

        case UNPACK_SEQUENCE: {
            auto n = oparg;
            while (n--) {
                stack.push();
            }
            stack.pop();
            break;
        }
        case UNPACK_EX: {
            auto n = (oparg & 0xff) + 1 + (oparg >> 8);
            while (n--) {
                stack.push();
            }
            stack.pop();
            break;
        }

        case GET_LEN:
            stack.push();
            stack.fetch(1);
            break;
        case MATCH_MAPPING:
        case MATCH_SEQUENCE:
            stack.push();
            stack.fetch(1);
            break;
        case MATCH_KEYS:
            stack.push();
            stack.push();
            stack.fetch(2);
            stack.fetch(1);
            break;
        case MATCH_CLASS:
            stack.push();
            stack.push();
            stack.popFromStack();
            stack.popFromStack();
            stack.pop();
            break;
        case COPY_DICT_WITHOUT_KEYS:
            stack.push();
            stack.fetch(1);
            stack.pop();
            break;

        case SETUP_FINALLY:
            block.branch_stack_difference = 6;
            break;
        case POP_BLOCK:
            break;
        case POP_EXCEPT:
            stack.popConsecutivelyFromStack(3);
            break;
        case JUMP_IF_NOT_EXC_MATCH:
            block.branch_stack_difference = 0;
            stack.pop();
            stack.pop();
            break;
        case RERAISE:
        case SETUP_WITH:
            block.branch_stack_difference = 5;
            stack.push();
            stack.push();
            stack.popFromStack();
            break;
        case WITH_EXCEPT_START:
            stack.push();
            stack.fetch(7);
            stack.fetch(3);
            stack.fetch(2);
            stack.fetch(1);
            break;
        case RAISE_VARARGS: {
            stack.popConsecutivelyFromStack(oparg);
            break;
        }

        case GEN_START:
            stack.pop();
            break;
        case YIELD_VALUE:
            stack.push();
            stack.popFromStack();
            break;
        case YIELD_FROM:
            stack.push();
            stack.popFromStack();
            stack.popFromStack();
            break;
        case GET_YIELD_FROM_ITER:
            stack.push();
            stack.pop();
            break;
        case GET_AWAITABLE:
            stack.push();
            stack.pop();
            break;
        case GET_AITER:
            stack.push();
            stack.pop();
            break;
        case GET_ANEXT:
            stack.push();
            stack.fetch(1);
            break;
        case END_ASYNC_FOR:
            stack.popConsecutivelyFromStack(7);
            break;
        case SETUP_ASYNC_WITH:
            block.branch_stack_difference = 5;
            stack.fetch(1);
            break;
        case BEFORE_ASYNC_WITH:
            stack.push();
            stack.push();
            stack.popFromStack();
            break;
        case EXTENDED_ARG:
        default:
            Py_UNREACHABLE();
            break;
        }
    }
}

struct PyEhBlock : PyAnalysisBlock {
    struct PyCodeBlock *declarer;
};

static void analyzeEHBlock(PyEhBlock &eb, unsigned chunks_for_nlocals, unsigned nesting_depth, PyCodeBlock &b) {
    assert(nesting_depth);
    if (b.worklist_link == &eb) {
        return;
    }
    b.worklist_link = &eb;
    for (auto i : IntRange(chunks_for_nlocals)) {
        eb.locals_touched[i] |= b.getBitArrayForLocalsEverDeleted()[i];
    }
    if (b.branch_block) {
        auto branch_block = b.has_try_entrance ? b.branch_block->branch_block : b.branch_block;
        analyzeEHBlock(eb, chunks_for_nlocals, nesting_depth, *static_cast<PyCodeBlock *>(branch_block));
    }
    if (b.fall_block) {
        nesting_depth = nesting_depth + b.has_try_entrance - b.has_try_exit;
        if (nesting_depth) {
            analyzeEHBlock(eb, chunks_for_nlocals, nesting_depth, b.fall());
        }
    }
}

void CompilationUnit::parsePyCode() {
    stack_height_arr.reserve(py_code.instrNum());
    abstract_stack.reserve(py_code->co_stacksize);
    auto chunks_for_nlocals = BitArray::chunkNumber(py_code->co_nlocals);

    const auto branch_offset_for_null_branch = py_code.instrNum();
    unsigned eh_block_num = 0;

    // Divide the sequence of instructions into basic blocks
    {
        ManagedBitArray block_boundaries(py_code.instrNum() + 1);
        block_num = 0;

        const auto first_instr = py_code.instrData();
        const auto &read_opcode = [first_instr](IntVPC vpc) {
            return _Py_OPCODE(first_instr[vpc]);
        };
        const auto &read_oparg = [first_instr](IntVPC vpc) {
            auto oparg = _Py_OPARG(first_instr[vpc]);
            for (unsigned shift = 0; vpc && _Py_OPCODE(first_instr[vpc - 1]) == EXTENDED_ARG;) {
                shift += PyCode::extended_arg_shift;
                oparg |= _Py_OPARG(first_instr[--vpc]) << shift;
            }
            return oparg;
        };

        // find out the boundaries of all Python blocks
        for (auto vpc : IntRange(py_code.instrNum())) {
            auto opcode = read_opcode(vpc);
            auto is_try_setup = isTryBlockSetup(opcode);
            auto is_rel_jmp = isRelativeJump(opcode);
            auto is_abs_jmp = isAbsoluteJmp(opcode);
            if (is_try_setup || is_rel_jmp || is_abs_jmp) {
                eh_block_num += is_try_setup;
                auto dest = read_oparg(vpc) + (is_abs_jmp ? 0 : vpc + 1);
                block_num += block_boundaries.testAndSet(dest);
                block_num += block_boundaries.testAndSet(vpc + 1);
            } else if (opcode == POP_BLOCK || opcode == YIELD_VALUE || opcode == YIELD_FROM) {
                // Note: According to _gen_throw in Objects/genobject.c,
                // the instruction immediately following YIELD_FROM can also be an entry point.
                // However, it will be treated as a special case with throwflag set,
                // so we don't care about it here.
                block_num += block_boundaries.testAndSet(vpc + (opcode != YIELD_FROM));
            }
        }
        block_num += block_boundaries.testAndSet(py_code.instrNum());
        block_num -= block_boundaries.get(0);
        block_boundaries.reset(0);

        // Create code blocks
        blocks.reserve(block_num);
        unsigned created_block_num = 0;
        IntVPC begin_vpc = 0;
        for (auto c : IntRange(BitArray::chunkNumber(py_code.instrNum() + 1))) {
            IntVPC end_vpc = c * BitArray::bits_per_chunk;
            for (auto bits = block_boundaries[c]; bits; bits >>= 1, end_vpc++) {
                if (!(bits & 1)) {
                    continue;
                }
                auto &b = blocks[created_block_num++];
                b.begin_vpc = begin_vpc;
                b.llvm_block = createBlock(nullptr, "PyBlock");
                b.handler_pc = nullptr;

                auto tail_opcode = read_opcode(end_vpc - 1);

                b.has_try_entrance = isTryBlockSetup(tail_opcode);
                b.has_try_exit = tail_opcode == POP_BLOCK;

                b.fall_block = isTerminator(tail_opcode) ? nullptr : &blocks[created_block_num];
                if (isAbsoluteJmp(tail_opcode)) {
                    b._branch_offset = read_oparg(end_vpc - 1);
                } else if (isRelativeJump(tail_opcode) || b.has_try_entrance) {
                    b._branch_offset = read_oparg(end_vpc - 1) + end_vpc;
                } else {
                    b._branch_offset = branch_offset_for_null_branch;
                }
                b.worklist_link = nullptr; // analyzeEHBlock(...) requires it to be initialized
                begin_vpc = end_vpc;
            }
        }
        assert(created_block_num == block_num);
    }

    DynamicArray<PyEhBlock> eh_blocks{eh_block_num};

    {
        // Allocate space for BitArray
        auto chunks_for_instr = BitArray::chunkNumber(py_code.instrNum());
        auto total_chunks = chunks_for_instr + 3 * chunks_for_nlocals * (block_num + eh_block_num);
        auto space = new BitArray::ChunkType[total_chunks]{};
        analysis_data.reset(space);
        redundant_loads.usePreallocatedSpace(space, chunks_for_instr);

        // Fix branch_block, and create exception handling blocks
        unsigned eh_block_index = 0;
        for (auto &b : PtrRange(blocks, block_num)) {
            if (b._branch_offset == branch_offset_for_null_branch) {
                b.branch_block = nullptr;
            } else {
                auto branch_block = std::lower_bound(blocks + 0, blocks + block_num, b._branch_offset,
                        [](const PyCodeBlock &block, IntVPC offset) { return block.begin_vpc < offset; });
                assert(branch_block != blocks + block_num && branch_block->begin_vpc == b._branch_offset);
                if (!b.has_try_entrance) {
                    b.branch_block = branch_block;
                } else {
                    auto &eb = eh_blocks[eh_block_index++];
                    b.branch_block = &eb;
                    eb.branch_block = branch_block;
                    eb.fall_block = nullptr;
                    eb.declarer = &b;
                    eb.locals_touched.usePreallocatedSpace(space, chunks_for_nlocals);
                    eb.locals_set.usePreallocatedSpace(space, chunks_for_nlocals);
                    eb.locals_input.usePreallocatedSpace(space, chunks_for_nlocals);
                }
            }
            b.locals_touched.usePreallocatedSpace(space, chunks_for_nlocals);
            b.locals_set.usePreallocatedSpace(space, chunks_for_nlocals);
            b.locals_input.usePreallocatedSpace(space, chunks_for_nlocals);
        }
        assert(eh_block_index == eh_block_num);
    }

    {
        // Intra-block analysis
        DynamicArray<IntVPC> timestamp_for_locals(py_code->co_nlocals);
        auto end_vpc = py_code.instrNum();
        // Trick: It is enough to reset timestamp_for_locals once, as the basic block is traversed from the back
        for (auto i : IntRange(py_code->co_nlocals)) {
            timestamp_for_locals[i] = end_vpc;
        }
        ReversedAbstractStack stack(abstract_stack + 0, py_code->co_stacksize);
        for (auto i = block_num; i--;) {
            stack.reset();
            performIntraBlockAnalysis(blocks[i], end_vpc, py_code, stack,
                    timestamp_for_locals, redundant_loads);
            end_vpc = blocks[i].begin_vpc;
            blocks[i].stack_effect = stack.height();
        }
    }

    // PyCodeBlock::worklist_link is used to mark whether a block has been visited
    for (auto &eb : PtrRange(eh_blocks, eh_block_num)) {
        analyzeEHBlock(eb, chunks_for_nlocals, 1, eb.declarer->fall());
    }

    // Inter-block analysis: initialize locals_input
    {
        PyAnalysisBlock *worklist_head = nullptr;
        const auto &append_to_worklist = [&](PyAnalysisBlock &b) {
            b.worklist_link = worklist_head;
            worklist_head = &b;
            for (auto i : IntRange(chunks_for_nlocals)) {
                b.locals_input[i] = ~BitArray::ChunkType{0};
            }
        };
        for (auto &eb : PtrRange(eh_blocks, eh_block_num)) {
            append_to_worklist(eb);
        }
        assert(block_num);
        for (auto i = block_num; --i;) {
            append_to_worklist(blocks[i]);
        }

        // for the entry block, nargs local variables are already defined
        blocks[0].worklist_link = worklist_head;
        size_t nargs = py_code->co_argcount + !!(py_code->co_flags & CO_VARARGS)
                + py_code->co_kwonlyargcount + !!(py_code->co_flags & CO_VARKEYWORDS);
        for (auto i : IntRange(chunks_for_nlocals)) {
            blocks[0].locals_input[i] = (BitArray::ChunkType{1} << nargs) - BitArray::ChunkType{1};
            nargs = nargs > BitArray::bits_per_chunk ? nargs - BitArray::bits_per_chunk : 0;
        }
    }

    // Inter-block analysis: solve locals_input with Fixed Point Algorithm
    {
        PyAnalysisBlock *worklist_head = &blocks[0];
        ManagedBitArray block_output(py_code->co_nlocals);
        do {
            auto &b = *worklist_head;
            worklist_head = b.worklist_link;
            b.worklist_link = &b; // mark it as not in worklist by pointing to self

            for (auto i : IntRange(chunks_for_nlocals)) {
                block_output[i] = (b.locals_input[i] & ~b.locals_touched[i]) | b.locals_set[i];
            }

            for (auto successor : {b.branch_block, b.fall_block}) {
                if (successor) {
                    bool any_update = false;
                    for (auto i : IntRange(chunks_for_nlocals)) {
                        auto &successor_input = successor->locals_input[i];
                        auto old_successor_input = successor_input;
                        successor_input &= block_output[i];
                        any_update |= successor_input != old_successor_input;
                    }
                    if (any_update && successor->worklist_link == successor) {
                        successor->worklist_link = worklist_head;
                        worklist_head = successor;
                    }
                }
            }
        } while (worklist_head);
    }

    // exception handling blocks are no longer needed
    for (auto &eb : PtrRange(eh_blocks, eh_block_num)) {
        assert(eb.declarer->branch_block == &eb);
        eb.declarer->branch_block = eb.branch_block;
    }

    // Inter-block analysis: solve initial_stack_height by Breadth-First Traversal
    {
        auto worklist_head = &blocks[0];
        blocks[0].initial_stack_height = !!(py_code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR));
        blocks[0].worklist_link = nullptr;
        do {
            auto &b = *worklist_head;
            worklist_head = static_cast<PyCodeBlock *>(b.worklist_link);
            b.worklist_link = nullptr;

            for (auto [possible_successor, difference] : std::initializer_list<std::tuple<PyAnalysisBlock *, int>>{
                    {b.branch_block, b.branch_stack_difference},
                    {b.fall_block, 0}
            }) {
                if (possible_successor) {
                    auto &successor = *static_cast<PyCodeBlock *>(possible_successor);
                    auto height = b.initial_stack_height + b.stack_effect + difference;
                    assert(0 <= height && height <= py_code->co_stacksize);
                    // a block's worklist_link points to itself if it is not visited
                    if (successor.worklist_link == &successor) {
                        successor.initial_stack_height = height;
                        successor.worklist_link = worklist_head;
                        worklist_head = &successor;
                    } else {
                        assert(successor.initial_stack_height == height);
                    }
                }
            }
        } while (worklist_head);
    }
}
