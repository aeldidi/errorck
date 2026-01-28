cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

project(errorck LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REQUIRED_LLVM_VERSION "18.1.8" CACHE STRING "Required LLVM/Clang version")
set(LLVM_ROOT "" CACHE PATH "Prefix containing lib/cmake/llvm and lib/cmake/clang")
set(LLVM_TAG "llvmorg-18.1.8" CACHE STRING "llvm-project git tag/commit to fetch")

if(LLVM_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${LLVM_ROOT}")
    set(LLVM_DIR "${LLVM_ROOT}/lib/cmake/llvm" CACHE PATH "" FORCE)
    set(Clang_DIR "${LLVM_ROOT}/lib/cmake/clang" CACHE PATH "" FORCE)
endif()

find_package(LLVM CONFIG QUIET)
find_package(Clang CONFIG QUIET)

if(LLVM_ROOT AND NOT (LLVM_FOUND AND Clang_FOUND))
    message(FATAL_ERROR
        "LLVM_ROOT is set to '${LLVM_ROOT}', but LLVM/Clang CMake packages were not found. "
        "Expected '${LLVM_ROOT}/lib/cmake/llvm' and '${LLVM_ROOT}/lib/cmake/clang'.")
endif()

if(LLVM_FOUND AND DEFINED LLVM_PACKAGE_VERSION AND NOT (LLVM_PACKAGE_VERSION VERSION_EQUAL REQUIRED_LLVM_VERSION))
    if(LLVM_ROOT)
        message(FATAL_ERROR "Found LLVM ${LLVM_PACKAGE_VERSION} at LLVM_ROOT, but required ${REQUIRED_LLVM_VERSION}.")
    else()
        set(LLVM_FOUND FALSE)
        set(Clang_FOUND FALSE)
    endif()
endif()

if(NOT (LLVM_FOUND AND Clang_FOUND))
    include(FetchContent)

    FetchContent_Declare(
        llvm_project
        GIT_REPOSITORY https://github.com/llvm/llvm-project.git
        GIT_TAG ${LLVM_TAG}
        SOURCE_SUBDIR llvm
    )

    set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra" CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(llvm_project)
endif()

add_executable(errorck "${CMAKE_CURRENT_LIST_DIR}/main.cpp")
target_compile_options(errorck PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
    -Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:
    /W4>)

# Match LLVM/Clang's RTTI and exception settings to avoid ABI/linker issues.
if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(errorck PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/GR->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>)
endif()
if(NOT LLVM_ENABLE_EH)
    target_compile_options(errorck PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/EHs->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions>)
endif()
target_include_directories(errorck PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(errorck PRIVATE ${LLVM_DEFINITIONS})
if(DEFINED CLANG_INCLUDE_DIRS)
    target_include_directories(errorck PRIVATE ${CLANG_INCLUDE_DIRS})
endif()
if(NOT (LLVM_FOUND AND Clang_FOUND))
    target_include_directories(errorck PRIVATE
        ${LLVM_SOURCE_DIR}/include
        ${LLVM_BINARY_DIR}/include
        ${LLVM_EXTERNAL_CLANG_SOURCE_DIR}/include
        ${LLVM_BINARY_DIR}/tools/clang/include
    )
endif()

# Query clang to match builtin headers with the linked LLVM/Clang libraries.
set(CLANG_RESOURCE_DIR "")
if(LLVM_FOUND)
    set(_clang_bin "${LLVM_TOOLS_BINARY_DIR}/clang")
    if(EXISTS "${_clang_bin}")
        execute_process(
            COMMAND "${_clang_bin}" -print-resource-dir
            OUTPUT_VARIABLE CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
endif()
if(NOT CLANG_RESOURCE_DIR)
    message(FATAL_ERROR
        "Failed to determine Clang resource dir from '${LLVM_TOOLS_BINARY_DIR}/clang'. "
        "Set CLANG_RESOURCE_DIR manually if clang is not present.")
endif()
target_compile_definitions(errorck PRIVATE CLANG_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")

if(TARGET clang-cpp)
    target_link_libraries(errorck PRIVATE clang-cpp)
else()
    target_link_libraries(errorck PRIVATE
        clangTooling
        clangFrontend
        clangSerialization
        clangDriver
        clangParse
        clangSema
        clangAnalysis
        clangRewrite
        clangAST
        clangASTMatchers
        clangEdit
        clangLex
        clangBasic
    )
endif()

enable_testing()
add_executable(errorck_test_runner tests/test_runner.cpp)
add_dependencies(errorck_test_runner errorck)
find_package(Threads REQUIRED)
target_link_libraries(errorck_test_runner PRIVATE Threads::Threads)
add_test(
    NAME errorck_tests
    COMMAND errorck_test_runner
        --build-dir ${CMAKE_BINARY_DIR}
        --tests-dir ${CMAKE_SOURCE_DIR}/tests
)
