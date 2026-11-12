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
add_library(svfanalyzer STATIC
    src/callgraph_analysis.cpp
    src/callgraph_output.cpp
    src/callgraph_tsv_sink.cpp
    src/callgraph_tree_sink.cpp
    src/function_filter.cpp)
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
# Bitcode entry-function lister: ./entry_extractor [options] <bitcode-file>
# Standalone LLVM tool: parses the module with IRReader, no SVF needed.
# function_filter.cpp supplies the DWARF-based app/library classification.
# ---------------------------------------------------------------------------
add_executable(entry_extractor
    src/entry_extractor.cpp
    src/function_filter.cpp)
target_include_directories(entry_extractor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(entry_extractor SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")
target_link_libraries(entry_extractor PRIVATE "${LLVM_LIB}")
set_target_properties(entry_extractor PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib;${LLVM_PREFIX}/lib")

# ---------------------------------------------------------------------------
# Class information tool: ./class_extractor [options] <input> [...]
#
# A single CLI entry point (class_extractor_cli.cpp) dispatches on the input
# file type:
#   .bc / .ll         -> class_extractor_bit_code.cpp (LLVM IR DWARF debug info)
#   C/C++ source      -> class_extractor_source.cpp   (libclang C API)
# Both backends fill the same classinfo::ClassReport;
# class_extractor_report.cpp renders it as JSON or text. The bitcode backend
# needs libLLVM, the source backend needs libclang (neither needs SVF).
# ---------------------------------------------------------------------------
# Use the libclang from LLVM_PREFIX (not a cached find_library result, which
# would go stale when LLVM_PREFIX changes); dev symlink first, soname second.
set(CLANG_LIB "")
foreach(candidate IN ITEMS
        "${LLVM_PREFIX}/lib/libclang.so"
        "${LLVM_PREFIX}/lib/libclang.so.1"
        "/usr/lib/x86_64-linux-gnu/libclang.so"
        "/usr/lib/x86_64-linux-gnu/libclang.so.1")
  if(EXISTS "${candidate}")
    set(CLANG_LIB "${candidate}")
    break()
  endif()
endforeach()
if(NOT CLANG_LIB)
  message(FATAL_ERROR
    "Could not find libclang in ${LLVM_PREFIX}/lib; install libclang-21-dev "
    "or pass -DLLVM_PREFIX=<path>.")
endif()

add_executable(class_extractor
    src/class_extractor_cli.cpp
    src/class_extractor_report.cpp
    src/class_extractor_bit_code.cpp
    src/class_extractor_source.cpp)
target_compile_options(class_extractor PRIVATE -Wall -Wextra)
target_include_directories(class_extractor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(class_extractor SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")
target_link_libraries(class_extractor PRIVATE
    "${LLVM_LIB}" "${CLANG_LIB}" ${CMAKE_DL_LIBS})
# libLLVM.so.21.1 is bundled in build/lib ($ORIGIN/lib); libclang.so is loaded
# from the LLVM toolchain's lib directory.
set_target_properties(class_extractor PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib;${LLVM_PREFIX}/lib")

# ---------------------------------------------------------------------------
# SQLite SQL-script extractor: ./sql_extractor [options] <source> [...]
#
# Walks C/C++ ASTs with the libclang C API to recover static SQL passed to
# sqlite3_exec / sqlite3_prepare_* (and any --api the user declares). Only
# libclang is needed: no SVF, no LLVM IR, no bitcode.
# ---------------------------------------------------------------------------
add_executable(sql_extractor
    src/sql_extractor_cli.cpp
    src/sql_extractor_source.cpp
    src/sql_extractor_report.cpp)
target_compile_options(sql_extractor PRIVATE -Wall -Wextra)
target_include_directories(sql_extractor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(sql_extractor SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")
target_link_libraries(sql_extractor PRIVATE "${CLANG_LIB}" ${CMAKE_DL_LIBS})
set_target_properties(sql_extractor PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib;${LLVM_PREFIX}/lib")

# ---------------------------------------------------------------------------
# Class/method change detector: ./class_diff [options] <old> <new> [-- args]
#
# Parses two C/C++ projects (source trees with libclang, .bc/.ll with LLVM IR
# + DWARF) and reports the classes that changed together with their added /
# removed / modified member functions. The shared model, both backends and the
# driver live in separate translation units.
# ---------------------------------------------------------------------------
add_executable(class_diff
    src/class_diff.cpp           # CLI driver / main
    src/class_diff_common.cpp    # shared model, discovery, diff + report writers
    src/class_diff_source.cpp    # libclang AST backend
    src/class_diff_bitcode.cpp   # LLVM IR + DWARF backend
    include/class_diff.h)
target_compile_options(class_diff PRIVATE -Wall -Wextra)
target_include_directories(class_diff PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(class_diff SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")
# libclang for the source backend, the monolithic libLLVM for the bitcode
# backend (IRReader, DebugInfo, Demangle).
target_link_libraries(class_diff PRIVATE
    "${LLVM_LIB}" "${CLANG_LIB}" ${CMAKE_DL_LIBS})
set_target_properties(class_diff PROPERTIES BUILD_RPATH "${LLVM_PREFIX}/lib")

# ---------------------------------------------------------------------------
# Bitcode SQLite SQL-script extractor: ./sql_bitcode_extractor [options] <module>
#
# Parses LLVM bitcode/IR with the IR reader and recovers the SQL argument of
# sqlite3_* calls: literals, global constants, functions returning a constant
# (interprocedurally) and branch fragments. A SVF/Andersen pointer-analysis
# layer resolves indirect-call targets (virtual dispatch, function pointers,
# dispatch tables, std::function/lambdas) into SQL candidates. Shared report
# output and CLI parsing live in sql_extractor_report.cpp.
# ---------------------------------------------------------------------------
add_executable(sql_bitcode_extractor
    src/sql_extractor_bit_code.cpp
    src/sql_extractor_report.cpp)
target_compile_options(sql_bitcode_extractor PRIVATE -Wall -Wextra)
target_include_directories(sql_bitcode_extractor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(sql_bitcode_extractor SYSTEM PRIVATE
    "${LLVM_INCLUDE_DIR}" ${SVF_INCLUDE_DIRS})
# Pull in SVF (headers + libraries: SvfCore/SvfLLVM, LLVM, z3) via the
# svfanalyzer library, which already configures them as PUBLIC.
target_link_libraries(sql_bitcode_extractor PRIVATE svfanalyzer
    "${LLVM_LIB}" ${CMAKE_DL_LIBS})
set_target_properties(sql_bitcode_extractor PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib;${LLVM_PREFIX}/lib;${SVF_BUILD_DIR}/lib")

# ---------------------------------------------------------------------------
# Native test program (builds with the default host compiler; no SVF).
# ---------------------------------------------------------------------------
add_executable(test_complex tests/test_complex.cpp)

# The web-service fixture is a real HTTP/1.1 server:
#   ./test_web_service [port] [max-requests]   (max-requests 0 = forever)
add_executable(test_web_service tests/test_web_service.cpp)

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

add_custom_command(OUTPUT "${TEST_BITCODE_DIR}/test_web_service.bc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BITCODE_DIR}"
    COMMAND "${LLVM_PREFIX}/bin/clang++" -std=c++17 -O0 -g -emit-llvm -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_service.cpp"
            -o "${TEST_BITCODE_DIR}/test_web_service.bc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_service.cpp"
    COMMENT "Compiling tests/test_web_service.cpp to LLVM bitcode")

# SQLite fixture: source extraction and bitcode extraction share this file.
add_custom_command(OUTPUT "${TEST_BITCODE_DIR}/test_sqlite_source.bc"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TEST_BITCODE_DIR}"
    COMMAND "${LLVM_PREFIX}/bin/clang++" -std=c++17 -O0 -g -emit-llvm -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sqlite_source.cpp"
            -o "${TEST_BITCODE_DIR}/test_sqlite_source.bc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sqlite_source.cpp"
    COMMENT "Compiling tests/test_sqlite_source.cpp to LLVM bitcode")

add_custom_target(bitcode ALL DEPENDS
    "${TEST_BITCODE_DIR}/test_complex.bc"
    "${TEST_BITCODE_DIR}/test_prog.bc"
    "${TEST_BITCODE_DIR}/test_web_service.bc"
    "${TEST_BITCODE_DIR}/test_sqlite_source.bc")

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

# class_extractor and entry_extractor resolve libLLVM through their
# $ORIGIN/lib rpath.
add_dependencies(class_extractor runtime-libs)
add_dependencies(entry_extractor runtime-libs)
# Source inputs are handled by the same binary through its libclang rpath.

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

# ---------------------------------------------------------------------------
# Regression tests (CTest): end-to-end assertions on the CLI, report layouts,
# library-code classification and filter handling. The driver compiles its own
# bitcode fixtures, so `ctest` needs no setup beyond a normal build.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  enable_testing()
  add_test(NAME analyzer_regression
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/regression_test.py"
              --analyzer "$<TARGET_FILE:clang_analyzer>"
              --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests"
              --clang "${LLVM_PREFIX}/bin/clang"
              --clangxx "${LLVM_PREFIX}/bin/clang++")
  set_tests_properties(analyzer_regression PROPERTIES TIMEOUT 600)

  # Entry-extractor suite: assembles its own IR fixtures and compiles the
  # runtime-registration C++ fixture, so ctest needs no extra setup.
  add_test(NAME entry_extractor_regression
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/entry_extractor_test.py"
              --extractor "$<TARGET_FILE:entry_extractor>"
              --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests"
              --clangxx "${LLVM_PREFIX}/bin/clang++"
              --llvm-as "${LLVM_PREFIX}/bin/llvm-as"
              --llvm-link "${LLVM_PREFIX}/bin/llvm-link")
  set_tests_properties(entry_extractor_regression PROPERTIES TIMEOUT 600)

  # SQL extractor suite: parses the C++ fixture with libclang and asserts
  # the JSON/text/SQL reports, API configuration and CLI error paths.
  add_test(NAME sql_extractor_regression
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/sql_extractor_test.py"
              --extractor "$<TARGET_FILE:sql_extractor>"
              --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  set_tests_properties(sql_extractor_regression PROPERTIES TIMEOUT 300)

  # Bitcode SQL extractor suite: parses the shared SQLite fixture and asserts
  # static SQL, function/indirect-call candidates and dynamic values.
  add_test(NAME sql_bitcode_extractor_regression
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/sql_bitcode_extractor_test.py"
              --extractor "$<TARGET_FILE:sql_bitcode_extractor>"
              --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests"
              --bitcode "${TEST_BITCODE_DIR}/test_sqlite_source.bc")
  set_tests_properties(sql_bitcode_extractor_regression PROPERTIES TIMEOUT 300)
endif()
