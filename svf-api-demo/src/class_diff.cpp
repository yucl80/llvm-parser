//===----------------------------------------------------------------------===//
// class_diff.cpp — driver / CLI for the class/method change detector.
//
//   ./class_diff [options] <old-project> <new-project> [-- <clang args>]
//
// Parses two C/C++ projects and reports the member functions of the classes
// that changed. Two interchangeable backends (source and bitcode) feed the
// very same snapshot/diff model implemented in class_diff_common.cpp; this
// file only parses the command line, selects the backend and drives the
// pipeline.
//
//   source  : class_diff_source.cpp  (libclang AST + source tokens)
//   bitcode : class_diff_bitcode.cpp (LLVM IR instruction fingerprints + DWARF)
//
// Classes are matched by qualified name and methods by signature, so an
// interface edit shows up as "modified" instead of added+removed.
//===----------------------------------------------------------------------===//

#include "class_diff.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace classdiff {

void printUsage(const char* Prog) {
    std::fprintf(
        stderr,
        "Usage: %s [options] <old-project> <new-project> [-- <clang args>]\n"
        "\n"
        "Compare two C/C++ projects and print the classes that changed together\n"
        "with their added / removed / modified member functions. Each argument\n"
        "may be a directory or a single file (.cpp/.h or .bc/.ll).\n"
        "\n"
        "Backends:\n"
        "  source              libclang AST + source tokens; comments,\n"
        "                      whitespace, formatting and line moves are ignored\n"
        "  bitcode             LLVM IR instruction fingerprints (SSA names and\n"
        "                      debug intrinsics ignored); with -g, DWARF also\n"
        "                      supplies class layout and virtual/access traits\n"
        "\n"
        "Options:\n"
        "  --input-type <type>    auto|source|bitcode [default: auto]\n"
        "  --format <text|json>   Report layout [default: text]\n"
        "  --output <file>        Write the report to <file> [default: stdout]\n"
        "  --std <standard>       Source: language standard without a\n"
        "                         compile_commands.json [default: c++17]\n"
        "  --ignore <fragment>    Skip paths containing <fragment> (repeatable)\n"
        "  --all-methods          Also list unchanged methods of changed classes\n"
        "  --include-library      Bitcode: keep std::, boost::, ... classes\n"
        "  --lib-prefix <ns>      Bitcode: extra library prefix (repeatable)\n"
        "  --exit-code            Return 1 when differences were found\n"
        "  --no-compilation-db    Source: ignore compile_commands.json\n"
        "  -- <clang args>        Source: extra arguments (-I, -D, ...) for both\n"
        "  -h, --help             Show this help\n"
        "\n"
        "auto: two .bc/.ll files, or directories holding only .bc/.ll, select\n"
        "bitcode; source-only inputs select source; mixed inputs need an explicit\n"
        "--input-type. Both projects should be compiled with the same flags\n"
        "(in particular -g) for meaningful bitcode comparisons. Bitcode only\n"
        "contains emitted IR: unused inline/template bodies have no instructions\n"
        "to compare and are checked at interface level only.\n",
        Prog);
}

bool parseArgs(int argc, char** argv, Options& O, std::string& Error) {
    bool afterDashDash = false;
    for (int I = 1; I < argc; ++I) {
        const std::string A = argv[I];
        if (afterDashDash) {
            O.clangArgs.push_back(A);
            continue;
        }
        if (A == "--") {
            afterDashDash = true;
        } else if (A == "-h" || A == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (A == "--format" || A == "--output" || A == "--std" ||
                   A == "--ignore" || A == "--input-type" ||
                   A == "--lib-prefix") {
            if (I + 1 >= argc) {
                Error = "missing value for " + A;
                return false;
            }
            const std::string V = argv[++I];
            if (A == "--format") {
                if (V != "text" && V != "json") {
                    Error = "unknown --format '" + V + "' (expected text or json)";
                    return false;
                }
                O.format = V;
            } else if (A == "--output") {
                O.output = V;
            } else if (A == "--std") {
                O.standard = V;
            } else if (A == "--input-type") {
                if (V != "auto" && V != "source" && V != "bitcode") {
                    Error = "unknown --input-type '" + V +
                            "' (expected auto, source or bitcode)";
                    return false;
                }
                O.inputType = V;
            } else if (A == "--lib-prefix") {
                O.libPrefixes.push_back(V);
            } else {
                O.ignores.push_back(V);
            }
        } else if (A == "--include-library") {
            O.includeLibrary = true;
        } else if (A == "--all-methods") {
            O.allMethods = true;
        } else if (A == "--exit-code") {
            O.exitCode = true;
        } else if (A == "--no-compilation-db") {
            O.useCompilationDB = false;
        } else if (startsWith(A, "--")) {
            Error = "unknown option: " + A;
            return false;
        } else {
            O.roots.push_back(A);
        }
    }
    if (O.roots.size() != 2) {
        Error = "expected exactly two project paths (old and new)";
        return false;
    }
    return true;
}

}  // namespace classdiff

int main(int argc, char** argv) {
    classdiff::Options O;
    std::string Error;
    if (!classdiff::parseArgs(argc, argv, O, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        classdiff::printUsage(argv[0]);
        return 1;
    }

    classdiff::Snapshot Old, New;
    if (!classdiff::initProject(O.roots[0], O, Old, Error) ||
        !classdiff::initProject(O.roots[1], O, New, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }
    classdiff::Backend B = classdiff::Backend::Source;
    if (!classdiff::detectBackend(O, Old, New, B, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }
    if (B == classdiff::Backend::Bitcode) {
        if (!O.clangArgs.empty() || O.standard != "c++17" ||
            !O.useCompilationDB)
            std::fprintf(stderr,
                         "[WARN] --std, --no-compilation-db and -- <clang args> "
                         "only apply to source inputs; ignored\n");
    } else if (O.includeLibrary || !O.libPrefixes.empty()) {
        std::fprintf(stderr,
                     "[WARN] --include-library and --lib-prefix only apply to "
                     "bitcode inputs; ignored\n");
    }

    if (B == classdiff::Backend::Bitcode) {
        classdiff::buildBitcodeSnapshot(Old, O);
        classdiff::buildBitcodeSnapshot(New, O);
        if (Old.hasDebugInfo != New.hasDebugInfo)
            std::fprintf(stderr,
                         "[WARN] only one project carries DWARF debug info; "
                         "both .bc sets should be built with the same flags\n");
    } else {
        const std::string ResourceDir = classdiff::detectResourceDir();
        const std::string ResourceArg =
            classdiff::hasArg(O.clangArgs, "-resource-dir") ||
                    ResourceDir.empty()
                ? std::string()
                : "-resource-dir=" + ResourceDir;
        classdiff::buildSnapshot(Old, O, ResourceArg);
        classdiff::buildSnapshot(New, O, ResourceArg);
    }

    const std::vector<classdiff::ClassDiff> Diffs =
        classdiff::diffSnapshots(Old, New, O.allMethods);

    FILE* Out = stdout;
    if (!O.output.empty()) {
        Out = std::fopen(O.output.c_str(), "w");
        if (!Out) {
            std::fprintf(stderr, "[ERROR] cannot write report to %s\n",
                         O.output.c_str());
            return 1;
        }
    }
    if (O.format == "json")
        classdiff::writeJson(Out, Old, New, Diffs, B);
    else
        classdiff::writeText(Out, Old, New, Diffs, B);
    if (Out != stdout)
        std::fclose(Out);

    size_t ChangedMethods = 0;
    for (const classdiff::ClassDiff& C : Diffs)
        for (const classdiff::MethodDiff& M : C.methods)
            if (M.change != classdiff::Change::Unchanged)
                ++ChangedMethods;
    std::fprintf(stderr,
                 "[class_diff] backend=%s | old: files=%zu classes=%zu | "
                 "new: files=%zu classes=%zu | changed_classes=%zu "
                 "changed_methods=%zu parse_errors=%zu\n",
                 classdiff::backendName(B), classdiff::fileCount(Old, B),
                 Old.classes.size(), classdiff::fileCount(New, B),
                 New.classes.size(), Diffs.size(), ChangedMethods,
                 Old.parseErrors + New.parseErrors);
    return O.exitCode && !Diffs.empty() ? 1 : 0;
}
