cmake_minimum_required(VERSION 3.20)
project(comPyler)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(CPYTHON_EXE CACHE FILEPATH "CPython executable file")
IF (CPYTHON_EXE STREQUAL "")
    message(FATAL_ERROR "CPYTHON_EXE is not set")
ELSE()
    message(STATUS "CPYTHON_EXE=${CPYTHON_EXE}")
ENDIF()

set(LLVM_ROOT CACHE PATH "LLVM 14 directory")
IF (LLVM_ROOT STREQUAL "")
    message(FATAL_ERROR "LLVM_ROOT is not set")
ELSE()
    message(STATUS "LLVM_ROOT=${LLVM_ROOT}")
ENDIF()

set(DIST ON CACHE BOOL "Build for distribution")
IF (DIST AND CMAKE_BUILD_TYPE MATCHES Debug)
    message(FATAL_ERROR "Build the distribution in debug mode, are you sure?")
ENDIF ()
message(STATUS "DIST=${DIST}")

set(ABLATION OFF CACHE BOOL "Build for ablation study")

find_package(LLVM 14 REQUIRED CONFIG PATHS ${LLVM_ROOT}/lib/cmake/llvm/ NO_DEFAULT_PATH)

execute_process(
        COMMAND ${CPYTHON_EXE} -c
        "from importlib.machinery import EXTENSION_SUFFIXES; print(EXTENSION_SUFFIXES[0], end='')"
        OUTPUT_VARIABLE EXTENSION_SUFFIX
)
execute_process(
        COMMAND ${CPYTHON_EXE} -c "import sysconfig; print(sysconfig.get_path('include'), end='')"
        OUTPUT_VARIABLE CPYTHON_INC_DIR
)

add_definitions(${LLVM_DEFINITIONS})
add_compile_definitions(Py_BUILD_CORE)
IF (ABLATION)
    add_compile_definitions(ABLATION_BUILD)
ENDIF ()
include_directories(${CPYTHON_INC_DIR} ${LLVM_INCLUDE_DIRS})
add_compile_options(
        -fno-semantic-interposition
        -fno-rtti -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
        -ffunction-sections -fdata-sections
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic> -Wall -Werror -Wno-unused-but-set-variable
)

link_directories(${LLVM_LIBRARY_DIRS})
add_link_options(-Wl,--exclude-libs,ALL,--gc-sections -fuse-ld=lld)

file(GLOB SRC CONFIGURE_DEPENDS "*.h" "*.cpp" "ported/*/*.c")

IF (DIST)
    IF (NOT (CMAKE_CXX_COMPILER_ID MATCHES Clang))
        message(FATAL_ERROR "Build in distribution mode only supports clang toolchain")
    ENDIF ()

    IF (ABLATION)
        llvm_map_components_to_libnames(LLVM_LIBS core native passes)
    ELSE ()
        llvm_map_components_to_libnames(LLVM_LIBS core native)
    ENDIF ()
    link_libraries(${LLVM_LIBS})
    add_link_options(-s)

    add_library(pgo_precursor SHARED EXCLUDE_FROM_ALL ${SRC})
    target_compile_options(pgo_precursor PRIVATE -fprofile-instr-generate)
    target_link_options(pgo_precursor PRIVATE -fprofile-instr-generate)

    set(PGO_DIR ${CMAKE_CURRENT_BINARY_DIR}/PGO)
    set(PGO_DATA ${PGO_DIR}/profdata)
    file(MAKE_DIRECTORY ${PGO_DIR})

    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH LLVM_PROFDATA)
    cmake_path(APPEND LLVM_PROFDATA "llvm-profdata")
    add_custom_command(
            OUTPUT ${PGO_DATA}
            COMMAND rm -f ${PGO_DIR}/*.raw
            COMMAND env -i LLVM_PROFILE_FILE=${PGO_DIR}/%p.raw PYTHONPATH=${PGO_DIR} ${CPYTHON_EXE} -m test --pgo
            COMMAND env -i ${LLVM_PROFDATA} merge ${PGO_DIR}/*.raw -o ${PGO_DATA}
            DEPENDS pgo_precursor
            USES_TERMINAL
    )
    add_custom_target(pgo_run DEPENDS ${PGO_DATA})

    add_library(compyler SHARED ${SRC})
    target_compile_options(compyler PRIVATE -fprofile-instr-use=${PGO_DATA})
    target_link_options(compyler PRIVATE -fprofile-instr-use=${PGO_DATA})

    set_target_properties(
            compyler pgo_precursor
            PROPERTIES
            PREFIX ""
            SUFFIX ${EXTENSION_SUFFIX}
            CXX_VISIBILITY_PRESET hidden
            INTERPROCEDURAL_OPTIMIZATION TRUE
    )
    set_target_properties(
            pgo_precursor
            PROPERTIES
            OUTPUT_NAME usercustomize
            LIBRARY_OUTPUT_DIRECTORY "${PGO_DIR}"
    )

    add_dependencies(compyler pgo_run)
ELSE ()
    link_libraries(LLVM-14)
    add_library(compyler SHARED ${SRC})
    set_target_properties(
            compyler
            PROPERTIES
            PREFIX ""
            SUFFIX ${EXTENSION_SUFFIX}
            CXX_VISIBILITY_PRESET hidden
    )
ENDIF ()
