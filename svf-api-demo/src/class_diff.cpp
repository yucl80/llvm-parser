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

void printUsage(const char* Prog, FILE* Out) {
    std::fprintf(
        Out,
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
        "  --allow-parse-errors   Continue even if a source/module failed to parse\n"
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

ParseResult parseArgs(int argc, char** argv, Options& O, std::string& Error) {
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
            printUsage(argv[0], stdout);
            return ParseResult::Help;
        } else if (A == "--format" || A == "--output" || A == "--std" ||
                   A == "--ignore" || A == "--input-type" ||
                   A == "--lib-prefix") {
            if (I + 1 >= argc) {
                Error = "missing value for " + A;
                return ParseResult::Error;
            }
            const std::string V = argv[++I];
            if (A == "--format") {
                if (V != "text" && V != "json") {
                    Error = "unknown --format '" + V + "' (expected text or json)";
                    return ParseResult::Error;
                }
                O.format = V;
            } else if (A == "--output") {
                O.output = V;
            } else if (A == "--std") {
                O.standard = V;
                O.standardSpecified = true;
            } else if (A == "--input-type") {
                if (V != "auto" && V != "source" && V != "bitcode") {
                    Error = "unknown --input-type '" + V +
                            "' (expected auto, source or bitcode)";
                    return ParseResult::Error;
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
        } else if (A == "--allow-parse-errors") {
            O.allowParseErrors = true;
        } else if (startsWith(A, "--")) {
            Error = "unknown option: " + A;
            return ParseResult::Error;
        } else {
            O.roots.push_back(A);
        }
    }
    if (O.roots.size() != 2) {
        Error = "expected exactly two project paths (old and new)";
        return ParseResult::Error;
    }
    return ParseResult::Ok;
}

}  // namespace classdiff

int main(int argc, char** argv) {
    classdiff::Options O;
    std::string Error;
    const classdiff::ParseResult Parsed =
        classdiff::parseArgs(argc, argv, O, Error);
    if (Parsed == classdiff::ParseResult::Help)
        return 0;
    if (Parsed == classdiff::ParseResult::Error) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        classdiff::printUsage(argv[0], stderr);
        return 1;
    }

    classdiff::Snapshot Old, New;
    std::string OldError, NewError;
    const bool OldOk = classdiff::initProject(O.roots[0], O, Old, OldError);
    const bool NewOk = classdiff::initProject(O.roots[1], O, New, NewError);
    if (!OldOk || !NewOk) {
        if (!OldOk)
            std::fprintf(stderr, "[ERROR] old project: %s\n", OldError.c_str());
        if (!NewOk)
            std::fprintf(stderr, "[ERROR] new project: %s\n", NewError.c_str());
        return 1;
    }

    classdiff::Backend B = classdiff::Backend::Source;
    if (!classdiff::detectBackend(O, Old, New, B, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }
    if (B == classdiff::Backend::Bitcode) {
        if (!O.clangArgs.empty() || O.standardSpecified || !O.useCompilationDB)
            std::fprintf(stderr,
                         "[WARN] --std, --no-compilation-db and -- <clang args> "
                         "only apply to source inputs; ignored\n");
    } else if (O.includeLibrary || !O.libPrefixes.empty()) {
        std::fprintf(stderr,
                     "[WARN] --include-library and --lib-prefix only apply to "
                     "bitcode inputs; ignored\n");
    }
    const size_t IgnoredFiles =
        B == classdiff::Backend::Bitcode
            ? Old.sources.size() + New.sources.size()
            : Old.bitcodes.size() + New.bitcodes.size();
    if (IgnoredFiles)
        std::fprintf(stderr,
                     "[WARN] --input-type %s: ignoring %zu %s input files\n",
                     classdiff::backendName(B), IgnoredFiles,
                     B == classdiff::Backend::Bitcode ? "source" : "bitcode");

    if (B == classdiff::Backend::Bitcode) {
        classdiff::buildBitcodeSnapshot(Old, O);
        classdiff::buildBitcodeSnapshot(New, O);
        if (Old.hasDebugInfo != New.hasDebugInfo) {
            std::fprintf(stderr,
                         "[ERROR] DWARF debug info differs between the two "
                         "projects (old=%s, new=%s); build both with the same "
                         "-g flags\n",
                         Old.hasDebugInfo ? "yes" : "no",
                         New.hasDebugInfo ? "yes" : "no");
            return 1;
        }
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

    const size_t ParseErrors = Old.parseErrors + New.parseErrors;
    if (ParseErrors && !O.allowParseErrors) {
        std::fprintf(stderr,
                     "[ERROR] %zu input file(s) failed to parse; use "
                     "--allow-parse-errors to continue anyway\n",
                     ParseErrors);
        return 1;
    }
    if (Old.classes.empty() || New.classes.empty()) {
        std::fprintf(stderr,
                     "[ERROR] no classes found in the %s snapshot(s) "
                     "(old=%zu, new=%zu)\n",
                     classdiff::backendName(B), Old.classes.size(),
                     New.classes.size());
        return 1;
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
    bool WriteFailed = false;
    if (std::fflush(Out) != 0 || std::ferror(Out))
        WriteFailed = true;
    if (Out != stdout && std::fclose(Out) != 0)
        WriteFailed = true;

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
                 New.classes.size(), Diffs.size(), ChangedMethods, ParseErrors);
    if (WriteFailed) {
        std::fprintf(stderr, "[ERROR] failed to write report\n");
        return 1;
    }
    return O.exitCode && !Diffs.empty() ? 1 : 0;
}
