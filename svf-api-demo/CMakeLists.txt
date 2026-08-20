cmake_minimum_required(VERSION 3.16)

project(svf_analyzer LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies
#
#   SVF_ROOT      SVF framework source tree containing svf/ + svf-llvm/ +
#                 Release-build/ (generated Util/config.h and libSvfCore/libSvfLLVM).
#                 Override with -DSVF_ROOT=<path> or the SVF_ROOT env var.
#
#   LLVM_PREFIX   LLVM 21 installation prefix (bin/, include/, lib/). The
#                 system-default clang 18 cannot read the .bc files produced by
#                 the LLVM-21 SVF build, so bitcode is generated with this
#                 toolchain and the analyzer links against libLLVM-21.
# ---------------------------------------------------------------------------
if(NOT DEFINED SVF_ROOT)
  if(DEFINED ENV{SVF_ROOT} AND NOT "$ENV{SVF_ROOT}" STREQUAL "")
    set(SVF_ROOT "$ENV{SVF_ROOT}")
  else()
    set(SVF_ROOT "/home/test/src/SVF")
  endif()
endif()
set(SVF_ROOT "${SVF_ROOT}" CACHE PATH "SVF framework build tree")

set(LLVM_PREFIX "/usr/lib/llvm-21" CACHE PATH "LLVM installation prefix")

set(SVF_INCLUDE_DIRS
    "${SVF_ROOT}/svf/include"
    "${SVF_ROOT}/svf-llvm/include"
    "${SVF_ROOT}/Release-build/include")
set(LLVM_INCLUDE_DIR "${LLVM_PREFIX}/include")

find_library(SVF_CORE_LIB NAMES SvfCore HINTS "${SVF_ROOT}/Release-build/lib")
find_library(SVF_LLVM_LIB NAMES SvfLLVM HINTS "${SVF_ROOT}/Release-build/lib")
find_library(LLVM_LIB NAMES LLVM-21 HINTS "${LLVM_PREFIX}/lib")

if(NOT SVF_CORE_LIB OR NOT SVF_LLVM_LIB OR NOT LLVM_LIB)
  message(FATAL_ERROR
    "Could not find SVF/LLVM libraries. Check SVF_ROOT (${SVF_ROOT}) and "
    "LLVM_PREFIX (${LLVM_PREFIX}).")
endif()

# ---------------------------------------------------------------------------
# Analysis core (no main) -- reusable library, the CLI driver links it.
# SVF/LLVM include dirs and libs are PUBLIC so consumers inherit them.
# ---------------------------------------------------------------------------
add_library(svfanalyzer STATIC src/callgraph_analysis.cpp)
target_include_directories(svfanalyzer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(svfanalyzer SYSTEM PUBLIC
    ${SVF_INCLUDE_DIRS}
    "${LLVM_INCLUDE_DIR}")
target_link_libraries(svfanalyzer PUBLIC
    "${SVF_CORE_LIB}" "${SVF_LLVM_LIB}" "${LLVM_LIB}"
    z3 pthread dl)

# ---------------------------------------------------------------------------
# Analyzer CLI driver: ./test_svf [flags] <bitcode-file>
# ---------------------------------------------------------------------------
add_executable(test_svf src/main.cpp)
target_link_libraries(test_svf PRIVATE svfanalyzer)

# ---------------------------------------------------------------------------
# Native test program (builds with the default host compiler; no SVF).
# ---------------------------------------------------------------------------
add_executable(test_complex tests/test_complex.cpp)

# ---------------------------------------------------------------------------
# Test-program bitcode, produced with the LLVM-21 toolchain (the system clang
# 18 cannot produce/read bitcode compatible with the LLVM-21 SVF build).
# ---------------------------------------------------------------------------
set(TEST_BITCODE_DIR "${CMAKE_BINARY_DIR}/tests")

add_custom_command(OUTPUT "${TEST_BITCODE_DIR}/test_complex.bc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BITCODE_DIR}"
    COMMAND "${LLVM_PREFIX}/bin/clang++" -std=c++17 -O0 -g -emit-llvm -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_complex.cpp"
            -o "${TEST_BITCODE_DIR}/test_complex.bc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_complex.cpp"
    COMMENT "Compiling tests/test_complex.cpp to LLVM bitcode")

add_custom_command(OUTPUT "${TEST_BITCODE_DIR}/test_prog.bc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BITCODE_DIR}"
    COMMAND "${LLVM_PREFIX}/bin/clang" -O0 -g -emit-llvm -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prog.c"
            -o "${TEST_BITCODE_DIR}/test_prog.bc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prog.c"
    COMMENT "Compiling tests/test_prog.c to LLVM bitcode")

add_custom_target(bitcode ALL DEPENDS
    "${TEST_BITCODE_DIR}/test_complex.bc"
    "${TEST_BITCODE_DIR}/test_prog.bc")

# ---------------------------------------------------------------------------
# Convenience target: run the analyzer on the test-complex bitcode with the
# default (flow-sensitive + context-sensitive) config.
#   cmake --build build --target analyze
# ---------------------------------------------------------------------------
add_custom_target(analyze
    COMMAND "${CMAKE_COMMAND}" -E env
            "LD_LIBRARY_PATH=${SVF_ROOT}/Release-build/lib:${LLVM_PREFIX}/lib"
            "$<TARGET_FILE:test_svf>" "${TEST_BITCODE_DIR}/test_complex.bc"
    DEPENDS test_svf bitcode
    COMMENT "Run test_svf on test_complex.bc (default config)")
