#ifndef COMPYLER_TYPES_H
#define COMPYLER_TYPES_H

#include "runtime.h"
#include "translated_result.h"

template <auto &symbol>
struct RuntimeSymbolEntry {};
template <auto &symbol, auto name>
struct RuntimeSymbolAndNameEntry {};

#ifdef DUMP_DEBUG_FILES
#define ENTRY(X) RuntimeSymbolAndNameEntry<X, std::to_array(#X)>
#else
#define ENTRY(X) RuntimeSymbolEntry<X>
#endif

// Put it here instead of inside RuntimeSymbolsHelper, otherwise it will crash gcc
template <auto &s>
struct SymbolIndexWrapper { size_t value; };

template <typename... T>
struct RuntimeSymbolsHelper : RuntimeSymbolsHelper<std::make_index_sequence<sizeof...(T)>, T...> {};

template <std::size_t... indices, auto &... symbols>
struct RuntimeSymbolsHelper<std::index_sequence<indices...>, RuntimeSymbolEntry<symbols>...> {
private:
    static constexpr std::tuple symbol_to_index{SymbolIndexWrapper<symbols>{indices}...};
public:
    using SymbolTypes = std::tuple<decltype(symbols)...>;

    inline static const std::array address_array{reinterpret_cast<void *>(&symbols)...};

    template <auto &s>
    static size_t getIndex() { return std::get<SymbolIndexWrapper<s>>(symbol_to_index).value; }
};

template <auto &... symbols, const auto... names>
struct RuntimeSymbolsHelper<RuntimeSymbolAndNameEntry<symbols, names>...> :
        RuntimeSymbolsHelper<RuntimeSymbolEntry<symbols>...> {
    inline static const std::array name_array{names.data()...};
};


using RuntimeSymbols = RuntimeSymbolsHelper<
        ENTRY(raiseUnboundError),

        ENTRY(handle_ROT_N),
        ENTRY(handle_LOAD_CLASSDEREF),
        ENTRY(handle_LOAD_GLOBAL),
        ENTRY(handle_STORE_GLOBAL),
        ENTRY(handle_DELETE_GLOBAL),
        ENTRY(handle_LOAD_NAME),
        ENTRY(handle_STORE_NAME),
        ENTRY(handle_DELETE_NAME),
        ENTRY(handle_LOAD_ATTR),
        ENTRY(handle_LOAD_METHOD),
        ENTRY(handle_STORE_ATTR),
        ENTRY(handle_BINARY_SUBSCR),
        ENTRY(handle_STORE_SUBSCR),
        ENTRY(handle_DELETE_SUBSCR),

        ENTRY(handle_UNARY_NOT),
        ENTRY(handle_UNARY_POSITIVE),
        ENTRY(handle_UNARY_NEGATIVE),
        ENTRY(handle_UNARY_INVERT),
        ENTRY(handle_BINARY_ADD),
        ENTRY(handle_INPLACE_ADD),
        ENTRY(handle_BINARY_SUBTRACT),
        ENTRY(handle_INPLACE_SUBTRACT),
        ENTRY(handle_BINARY_MULTIPLY),
        ENTRY(handle_INPLACE_MULTIPLY),
        ENTRY(handle_BINARY_FLOOR_DIVIDE),
        ENTRY(handle_INPLACE_FLOOR_DIVIDE),
        ENTRY(handle_BINARY_TRUE_DIVIDE),
        ENTRY(handle_INPLACE_TRUE_DIVIDE),
        ENTRY(handle_BINARY_MODULO),
        ENTRY(handle_INPLACE_MODULO),
        ENTRY(handle_BINARY_POWER),
        ENTRY(handle_INPLACE_POWER),
        ENTRY(handle_BINARY_MATRIX_MULTIPLY),
        ENTRY(handle_INPLACE_MATRIX_MULTIPLY),
        ENTRY(handle_BINARY_LSHIFT),
        ENTRY(handle_INPLACE_LSHIFT),
        ENTRY(handle_BINARY_RSHIFT),
        ENTRY(handle_INPLACE_RSHIFT),
        ENTRY(handle_BINARY_AND),
        ENTRY(handle_INPLACE_AND),
        ENTRY(handle_BINARY_OR),
        ENTRY(handle_INPLACE_OR),
        ENTRY(handle_BINARY_XOR),
        ENTRY(handle_INPLACE_XOR),
        ENTRY(handle_COMPARE_OP),
        ENTRY(handle_CONTAINS_OP),

        ENTRY(handle_CALL_FUNCTION),
        ENTRY(handle_CALL_METHOD),
        ENTRY(handle_CALL_FUNCTION_KW),
        ENTRY(handle_CALL_FUNCTION_EX),
        ENTRY(handle_MAKE_FUNCTION),
        ENTRY(handle_LOAD_BUILD_CLASS),

        ENTRY(handle_IMPORT_NAME),
        ENTRY(handle_IMPORT_FROM),
        ENTRY(handle_IMPORT_STAR),

        ENTRY(handle_GET_ITER),
        ENTRY(handle_FOR_ITER),

        ENTRY(handle_BUILD_STRING),
        ENTRY(handle_BUILD_TUPLE),
        ENTRY(handle_BUILD_LIST),
        ENTRY(handle_BUILD_SET),
        ENTRY(handle_BUILD_MAP),
        ENTRY(handle_BUILD_CONST_KEY_MAP),
        ENTRY(handle_LIST_APPEND),
        ENTRY(handle_SET_ADD),
        ENTRY(handle_MAP_ADD),
        ENTRY(handle_LIST_EXTEND),
        ENTRY(handle_SET_UPDATE),
        ENTRY(handle_DICT_UPDATE),
        ENTRY(handle_DICT_MERGE),
        ENTRY(handle_LIST_TO_TUPLE),

        ENTRY(handle_FORMAT_VALUE),
        ENTRY(handle_BUILD_SLICE),
        ENTRY(handle_RAISE_VARARGS),
        ENTRY(handle_SETUP_ANNOTATIONS),
        ENTRY(handle_PRINT_EXPR),

        ENTRY(handle_UNPACK_EX),
        ENTRY(handle_UNPACK_SEQUENCE),

        ENTRY(hanlde_GET_LEN),
        ENTRY(hanlde_MATCH_KEYS),
        ENTRY(hanlde_MATCH_CLASS),
        ENTRY(handle_COPY_DICT_WITHOUT_KEYS),

        ENTRY(PyFrame_BlockSetup),
        ENTRY(PyFrame_BlockPop),
        ENTRY(handle_POP_EXCEPT),
        ENTRY(handle_JUMP_IF_NOT_EXC_MATCH),
        ENTRY(handle_RERAISE),
        ENTRY(handle_SETUP_WITH),
        ENTRY(handle_WITH_EXCEPT_START),

        ENTRY(handle_YIELD_VALUE),
        ENTRY(handle_YIELD_FROM),
        ENTRY(handle_GET_YIELD_FROM_ITER),
        ENTRY(handle_GET_AWAITABLE),
        ENTRY(handle_GET_AITER),
        ENTRY(handle_GET_ANEXT),
        ENTRY(handle_END_ASYNC_FOR),
        ENTRY(handle_BEFORE_ASYNC_WITH),

        ENTRY(castPyObjectToBool),

        ENTRY(handleEvalBreaker),

        ENTRY(_Py_FalseStruct),
        ENTRY(_Py_TrueStruct),
        ENTRY(PyExc_AssertionError)
#ifdef NON_INLINE_RC
        ,
        ENTRY(_Py_IncRef),
        ENTRY(_Py_DecRef)
#endif
>;

template <typename T, typename = void>
struct TypeNormalizer;

template <typename T>
using NormalizedType = typename TypeNormalizer<std::remove_cv_t<T>>::type;

template <typename T>
struct TypeNormalizer<T, std::enable_if_t<std::is_same_v<T, bool>>> {
    using type = bool;
};

template <typename T>
struct TypeNormalizer<T, std::enable_if_t<std::is_enum_v<T>>> {
    using type = std::make_signed_t<std::underlying_type_t<T>>;
};

template <typename T>
struct TypeNormalizer<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    using type = std::make_signed_t<T>;
};

template <typename T>
struct TypeNormalizer<T, std::enable_if_t<!std::is_scalar_v<T> && !std::is_function_v<T>>> {
    using type = void;
};

template <typename T>
struct TypeNormalizer<T *> {
    using type = void *;
};

template <typename Ret, typename... Args>
struct TypeNormalizer<Ret(Args...)> {
    using type = NormalizedType<Ret>(NormalizedType<Args>...);
};

#endif
