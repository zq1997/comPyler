import struct
import os
from opcode import EXTENDED_ARG, HAVE_ARGUMENT, opname, cmp_op, \
    haslocal, hasconst, hasname, hasfree, hasjabs, hasjrel, hascompare
import compyler


def debug_compile(infix=None):
    if not hasattr(compyler, '_debug_compile'):
        return compyler.compile

    def decorator(func):
        co = func.__code__
        py_dir, py_file = os.path.split(co.co_filename)
        pydis_dir = os.path.join(py_dir, '__pycache__')
        save_name = '%s.%s' % (os.path.splitext(py_file)[0], co.co_name if infix is None else infix)

        if not os.path.exists(pydis_dir):
            os.makedirs(pydis_dir, exist_ok=True)
        with open(os.path.join(pydis_dir, save_name + '.pydis'), 'wt') as f:
            disassemble_code(f, co, False)

        save_path_base = os.path.join(pydis_dir, save_name)
        compyler._debug_compile((
            co,
            os.fsencode(save_name + '.pydis'),
            os.fsencode(save_path_base + ".ll"),
            os.fsencode(save_path_base + ".o")
        ))
        return func

    return decorator


def unpack_opargs(code):
    extended_arg = 0
    for i, (code_unit,) in enumerate(struct.iter_unpack('=H', code)):
        op = code_unit & 0xff
        short_arg = code_unit >> 8
        arg = short_arg | extended_arg
        extended_arg = (arg << 8) if op == EXTENDED_ARG else 0
        yield i, op, short_arg, arg


OPNAME_WIDTH = max(len(x) for x in opname)


def disassemble_code(file, co, recursively):
    print('# %s @ line %d' % (co.co_name, co.co_firstlineno), file=file)
    lastline = None
    linestarts = {}
    for start, end, line in co.co_lines():
        if line is not None and line != lastline:
            lastline = linestarts[start] = line

    lineno_width = len(str(max(linestarts.values())))
    offset_width = len(str(len(co.co_code) - 2))
    line_format = f'._%{lineno_width}s %2s %{offset_width}d %02x%02x    %-{OPNAME_WIDTH}s %s'

    labels = set()
    for offset, op, _, arg in unpack_opargs(co.co_code):
        if arg is not None:
            if op in hasjrel:
                labels.add(offset + 1 + arg)
            elif op in hasjabs:
                labels.add(arg)

    for offset, op, short_arg, arg in unpack_opargs(co.co_code):
        starts_line = linestarts.get(offset * 2, None)
        if starts_line is None:
            starts_line = ''
        else:
            starts_line = str(starts_line)

        if op < HAVE_ARGUMENT:
            arg_repr = ''
        elif op in haslocal:
            arg_repr = co.co_varnames[arg]
        elif op in hasconst:
            arg_repr = repr(co.co_consts[arg])
            # if len(arg_repr) > 100:
            #     arg_repr = arg_repr[:100] + '...'
        elif op in hasname:
            arg_repr = co.co_names[arg]
        elif op in hasfree:
            ncells = len(co.co_cellvars)
            arg_repr = co.co_cellvars[arg] if arg < ncells else co.co_freevars[arg - ncells]
        elif op in hascompare:
            arg_repr = cmp_op[arg]
        elif op in hasjabs:
            arg_repr = '<to %d>' % arg
        elif op in hasjrel:
            arg_repr = '<to %d>' % (offset + 1 + arg)
        else:
            arg_repr = '<%d>' % arg

        print(line_format % (
            '' if starts_line is None else str(starts_line),
            '>>' if offset in labels else '',
            offset,
            op,
            short_arg,
            opname[op],
            arg_repr
        ), file=file)

    if recursively:
        for sub_co in co.co_consts:
            if type(sub_co) is type(co):
                print()
                disassemble_code(sub_co, file, recursively)


if __name__ == '__main__':
    import sys

    if len(sys.argv) != 2:
        print('wrong argument', file=sys.stderr)
    else:
        with open(sys.argv[1]) as src_file:
            code = compile(src_file.read(), sys.argv[1], "exec")
        disassemble_code(sys.stdout, code, True)
