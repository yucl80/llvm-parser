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
	  set(SVF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../SVF")
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
# Analyzer CLI driver: ./clang_analyzer [flags] <bitcode-file>
# ---------------------------------------------------------------------------
add_executable(clang_analyzer src/main.cpp)
target_link_libraries(clang_analyzer PRIVATE svfanalyzer)

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
# Deployable runtime (self-contained; no LD_LIBRARY_PATH needed on the target)
#
# clang_analyzer is linked with an $ORIGIN-relative rpath so the SVF/LLVM/z3
# shared libraries can live in ./lib next to the binary. Copy the whole
# build/deploy folder to any x86-64 Linux and run -- no installs or env vars.
# ---------------------------------------------------------------------------
set_target_properties(clang_analyzer PROPERTIES BUILD_RPATH "\$ORIGIN/lib")

# Runtime SONAMEs the loader resolves (the versioned .so.3/.so.21.1 files, not
# the dev symlinks). cmake -E copy dereferences symlinks, so copying these
# names yields exactly the file the loader looks up at runtime.
set(RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
# Exact SONAME file, not a -l style name, so find_file (not find_library).
find_file(Z3_RUNTIME_LIB NAMES libz3.so.4 libz3.so.3
    HINTS "/lib/x86_64-linux-gnu" "/usr/lib/x86_64-linux-gnu")

# extapi.bc is SVF's external-API model bitcode; SVF locates it relative to
# the loaded libSvfCore.so (ExtAPI.cpp item 6), so it must ship beside it.
set(RUNTIME_LIBS
    "${SVF_ROOT}/Release-build/lib/libSvfCore.so.3"
    "${SVF_ROOT}/Release-build/lib/libSvfLLVM.so.3"
    "${LLVM_PREFIX}/lib/libLLVM.so.21.1"
    "${Z3_RUNTIME_LIB}"
    "${SVF_ROOT}/Release-build/lib/extapi.bc")

# Copy the runtime libs into <build>/lib/ next to the analyzer binary.
set(RUNTIME_LIB_FILES)
foreach(lib IN LISTS RUNTIME_LIBS)
    if(NOT lib)
        message(FATAL_ERROR "Could not locate a required runtime shared library")
    endif()
    get_filename_component(libname "${lib}" NAME)
    set(out "${RUNTIME_LIB_DIR}/${libname}")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RUNTIME_LIB_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${lib}" "${out}"
        DEPENDS "${lib}"
        COMMENT "Copying ${libname} for deployment")
    list(APPEND RUNTIME_LIB_FILES "${out}")
endforeach()
add_custom_target(runtime-libs ALL DEPENDS ${RUNTIME_LIB_FILES})

# Assemble a clean self-contained deployment folder: clang_analyzer + lib/.
add_custom_target(deploy
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/deploy"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:clang_analyzer>" "${CMAKE_BINARY_DIR}/deploy/clang_analyzer"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${RUNTIME_LIB_DIR}" "${CMAKE_BINARY_DIR}/deploy/lib"
    DEPENDS clang_analyzer runtime-libs
    COMMENT "Self-contained deployment ready at ${CMAKE_BINARY_DIR}/deploy")

# ---------------------------------------------------------------------------
# Convenience target: run the analyzer on the test-complex bitcode with the
# default (flow-sensitive + context-sensitive) config.
#   cmake --build build --target analyze
# (BUILD_RPATH + runtime-libs make LD_LIBRARY_PATH unnecessary.)
# ---------------------------------------------------------------------------
add_custom_target(analyze
    COMMAND "$<TARGET_FILE:clang_analyzer>" "${TEST_BITCODE_DIR}/test_complex.bc"
    DEPENDS clang_analyzer bitcode runtime-libs
    COMMENT "Run clang_analyzer on test_complex.bc (default config)")
