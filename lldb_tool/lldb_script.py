import os
import importlib.util
import sys

import lldb

import official_gdb_script
import gdb


def on_code_loaded(frame: lldb.SBFrame, bp_loc, extra_args, internal_dict):
    bin_addr = frame.FindVariable('bin_addr').unsigned
    py_code = official_gdb_script.PyCodeObjectPtr.from_pyobject_ptr(gdb.Value(frame.FindVariable('py_code')))

    co_filename = py_code.pyop_field('co_filename').proxyval(None)
    if not os.path.isfile(co_filename):
        return
    co_name = py_code.pyop_field('co_name').proxyval(None)
    py_dir, py_file = os.path.split(co_filename)
    obj_file = os.path.join(py_dir, '__pycache__', '%s.%s.o' % (os.path.splitext(py_file)[0], co_name))
    if not os.path.isfile(obj_file):
        return
    if os.path.getmtime(obj_file) < os.path.getmtime(co_filename):
        print("debug files is are out of date", file=sys.stderr)
        return

    target = frame.thread.process.target
    mod = target.AddModule(obj_file, None, None)
    target.SetModuleLoadAddress(mod, bin_addr)
    print('jit module loaded: 0x%x ' % bin_addr, mod)


def pretty_print(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    if not var.type.IsValid():
        result.SetError('invalid expression')
        return
    printer = official_gdb_script.PyObjectPtrPrinter(gdb.Value(var))
    if printer is not None:
        result.Print(printer.to_string() + '\n')
        return
    result.SetError('no pretty printer for ' + str(var))


def print_ref(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    var = gdb.get_lldb_frame().EvaluateExpression(command)
    cast_type = official_gdb_script.PyObjectPtr.get_gdb_type()
    ob_refcnt = int(official_gdb_script.PyObjectPtr(gdb.Value(var), cast_type).field('ob_refcnt'))
    result.Print(f'{ob_refcnt} refs\n')


def print_stack(debugger, command, result: lldb.SBCommandReturnObject, internal_dict):
    frame = official_gdb_script.Frame.get_selected_frame()
    while frame and not frame.is_evalframe():
        frame = frame.older()
    if frame:
        frame = frame.get_pyop()
    else:
        result.SetError('no frame')
        return
    stack_size = official_gdb_script.PyCodeObjectPtr.from_pyobject_ptr(frame.field('f_code')).field('co_stacksize')
    limit = official_gdb_script.int_from_int(stack_size)
    limit = min(limit, int(command)) if command else limit
    stack = frame.field('f_valuestack')
    stack_address = official_gdb_script.int_from_int(stack)
    cast_type = official_gdb_script.PyObjectPtr.get_gdb_type()
    for i in range(limit):
        element = stack[i]
        try:
            ref = int(official_gdb_script.PyObjectPtr(element, cast_type).field('ob_refcnt'))
            assert ref > 0
            text = official_gdb_script.PyObjectPtr.from_pyobject_ptr(element).get_truncated_repr(128)
        except (official_gdb_script.NullPyObjectPtr, AssertionError):
            result.AppendMessage('%2d ----' % i)
        else:
            result.AppendMessage('%2d %016x [%4d] %s\n' % (i, stack_address + 8 * i, ref, text))


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    if '_already_loaded' in internal_dict:
        importlib.reload(gdb)
        importlib.reload(official_gdb_script)
    else:
        internal_dict['_already_loaded'] = True
        target: lldb.SBTarget = debugger.GetDummyTarget()
        sp: lldb.SBBreakpoint = target.BreakpointCreateByName('notifyCodeLoaded')
        sp.SetScriptCallbackFunction(f'{__name__}.on_code_loaded')
        sp.SetAutoContinue(True)

        debugger.HandleCommand(f'command script add -o -f {__name__}.pretty_print pp')
        debugger.HandleCommand(f'command script add -o -f {__name__}.print_ref py-ref')
        debugger.HandleCommand(f'command script add -o -f {__name__}.print_stack py-stack')

    official_gdb_script.chr = lambda x: chr(int(x))
    official_gdb_script.EVALFRAME = '_PyEval_EvalFrame'
