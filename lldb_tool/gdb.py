import re

import lldb


def get_lldb_debugger() -> lldb.SBDebugger:
    return lldb.debugger


def get_lldb_target() -> lldb.SBTarget:
    return get_lldb_debugger().GetSelectedTarget()


def get_lldb_process() -> lldb.SBProcess:
    return get_lldb_target().GetProcess()


def get_lldb_thread() -> lldb.SBThread:
    return get_lldb_process().GetSelectedThread()


def get_lldb_frame() -> lldb.SBFrame:
    res = lldb.SBCommandReturnObject()
    get_lldb_debugger().GetCommandInterpreter().HandleCommand('frame info', res)
    frame_id = int(re.search(r'frame #(\d+)', res.GetOutput()).group(1))
    return get_lldb_thread().GetFrameAtIndex(frame_id)
    # return get_lldb_thread().GetSelectedFrame()


class Frame:
    class FrameType:
        def __init__(self, outer: 'Frame'):
            self.outer = outer

        def __eq__(self, other):
            f = self.outer.lldb_frame
            if other == NORMAL_FRAME:
                return not f.is_inlined and not f.IsArtificial()
            if other == INLINE_FRAME:
                return f.is_inlined

    def __init__(self, lldb_frame: lldb.SBFrame):
        assert lldb_frame.IsValid()
        self.lldb_frame = lldb_frame

    def __str__(self):
        return str(self.lldb_frame)

    def type(self):
        return Frame.FrameType(self)

    def name(self):
        return self.lldb_frame.name

    def older(self):
        p = self.lldb_frame.parent
        return Frame(p) if p.IsValid() else None

    def read_var(self, name):
        return Value(self.lldb_frame.FindVariable(name))


NORMAL_FRAME = 'NORMAL_FRAME'
INLINE_FRAME = 'INLINE_FRAME'


def selected_frame():
    return Frame(get_lldb_frame())


class Command:
    def invoke(self, args, from_tty):
        raise NotImplementedError

    def __init__(self, name, command_class, completer_class=None, prefix=None):
        def lldb_command_function(debugger, command, result, internal_dict):
            self.invoke(command, True)

        self.__class__.wrapped_invoke = lldb_command_function
        lldb.debugger.HandleCommand('command script add -o -f %s.%s.wrapped_invoke %s' %
                                    (self.__class__.__module__, self.__class__.__qualname__, name))


COMMAND_DATA = None
COMMAND_STACK = None
COMMAND_FILES = None
COMPLETE_NONE = None


class Field:
    def __init__(self, lldb_field: lldb.SBTypeMember, parent: 'Type'):
        assert lldb_field.IsValid()
        self.bitpos = lldb_field.bit_offset
        self.enumval = None
        self.name = lldb_field.name
        self.artificial = False
        self.is_base_class = False
        self.bitsize = lldb_field.bitfield_bit_size if lldb_field.is_bitfield else 0
        self.type = Type(lldb_field.type)
        self.parent_type = parent


TYPE_CODE_PTR = lldb.eTypeClassPointer


class Type:
    def __init__(self, lldb_type: lldb.SBType):
        assert lldb_type.IsValid()
        self.lldb_type = lldb_type
        self.code = lldb_type.GetTypeClass()

    def pointer(self):
        return Type(self.lldb_type.GetPointerType())

    def sizeof(self):
        return self.lldb_type.size

    def fields(self):
        return [Field(f, self) for f in self.lldb_type.fields]

    def unqualified(self):
        return Type(self.lldb_type.GetUnqualifiedType())

    def target(self):
        if self.lldb_type.IsPointerType():
            return Type(self.lldb_type.GetPointeeType())
        if self.lldb_type.IsArrayType():
            return Type(self.lldb_type.GetArrayElementType())
        if self.lldb_type.IsFunctionType():
            return Type(self.lldb_type.GetFunctionReturnType())
        return None

    def __str__(self):
        return self.lldb_type.name


class Value:
    def __init__(self, lldb_value: lldb.SBValue):
        assert lldb_value.IsValid()
        self.lldb_value = lldb_value
        self.type = Type(lldb_value.type)
        self.is_optimized_out = False

    def __getitem__(self, item):
        try:
            if isinstance(item, str):
                return Value(self.lldb_value.GetChildMemberWithName(item))
            elif isinstance(item, int):
                return Value(self.lldb_value.GetValueForExpressionPath('[%d]' % item))
        except AssertionError:
            raise RuntimeError
        assert False

    def __int__(self):
        bt = self.type.lldb_type.GetBasicType()
        if bt in (lldb.eBasicTypeSignedChar, lldb.eBasicTypeSignedWChar, lldb.eBasicTypeShort,
                  lldb.eBasicTypeInt, lldb.eBasicTypeLong, lldb.eBasicTypeLongLong, lldb.eBasicTypeInt128):
            return self.lldb_value.signed
        return self.lldb_value.unsigned

    def __add__(self, other):
        if self.type.lldb_type.is_pointer:
            return Value(self.lldb_value.GetValueForExpressionPath('[%d]' % other).address_of)

    def __str__(self):
        return str(self.lldb_value)

    def cast(self, to_type: Type):
        return Value(self.lldb_value.Cast(to_type.lldb_type))

    def dereference(self):
        return Value(self.lldb_value.Dereference())

    def string(self):
        assert str(self.type.target().unqualified()) == 'char'
        summary = self.lldb_value.summary
        assert type(summary) is str
        return eval(summary)

    @property
    def address(self):
        return Value(self.lldb_value.address_of)


class Symbol:
    def __init__(self, lldb_symbol: lldb.SBSymbol):
        assert lldb_symbol.IsValid()
        self.lldb_symbol = lldb_symbol

    def value(self) -> Value:
        assert self.lldb_symbol.type == lldb.eSymbolTypeData
        return get_lldb_target().FindFirstGlobalVariable(self.lldb_symbol.name)


def lookup_type(name: str) -> Type:
    lldb_type = lldb.debugger.GetSelectedTarget().FindFirstType(name)
    return Type(lldb_type)


def lookup_global_symbol(name: str) -> Symbol:
    symbols = get_lldb_target().FindSymbols(name).symbols
    assert len(symbols) == 1
    return Symbol(symbols[0])


def current_objfile():
    return None


class pretty_printers:
    @staticmethod
    def append(_): pass


error = AssertionError
