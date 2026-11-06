//===----------------------------------------------------------------------===//
// class_cli.cpp — single entry point of the class-information tool.
//
//   ./class_extractor [options] <input> [more inputs ...] [-- <clang args>]
//
// The input file type selects the extraction backend:
//   * .bc / .ll (LLVM bitcode or textual IR) -> DWARF debug info (LLVM API)
//   * .c / .cpp / .h / ... (C/C++ sources)   -> libclang C API
//   * other extensions                      -> detected from file contents
//   * --input-type bitcode|source            -> explicit override
//
// Both backends fill a classinfo::ClassReport; the report is rendered here in
// the requested format, independently of which backend produced it.
//===----------------------------------------------------------------------===//

#include "class_report.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace classinfo;

namespace {

void printUsage(const char* prog) {
    std::fprintf(
        stderr,
        "Usage: %s [options] <input> [more inputs ...] [-- <clang args>]\n"
        "\n"
        "Extract classes, member functions, fields and inheritance from either\n"
        "an LLVM bitcode file (DWARF debug info, compile with -g) or C/C++\n"
        "source files (libclang). The backend is chosen from the input type.\n"
        "\n"
        "Inputs:\n"
        "  .bc, .ll                 LLVM bitcode / textual IR (one file)\n"
        "  .c, .cc, .cpp, .h, ... C/C++ source (several files allowed)\n"
        "  --input-type <type>     Force the backend: auto|bitcode|source\n"
        "\n"
        "Options:\n"
        "  --format <json|text>    Report layout [default: json]\n"
        "  --output <file>         Write the report to <file> [default: stdout]\n"
        "  --include-library       Bitcode: keep classes defined outside the\n"
        "                          application's source tree [default: hidden]\n"
        "  --all, -a               Source: also report classes from headers\n"
        "                          (system headers are always skipped)\n"
        "  -- <clang args>         Source: pass the remaining arguments to\n"
        "                          clang verbatim\n"
        "  -h, --help              Show this help\n",
        prog);
}

bool parseArgs(int argc, char** argv, ClassOptions& options,
               std::string& error) {
    bool afterDashDash = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (afterDashDash) {
            options.clangArgs.push_back(arg);
        } else if (arg == "--") {
            afterDashDash = true;
        } else if (arg == "--format" || arg == "--output" ||
                   arg == "--input-type") {
            if (i + 1 >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--format") {
                if (value != "json" && value != "text") {
                    error = "unknown --format '" + value +
                            "' (expected json or text)";
                    return false;
                }
                options.format = value;
            } else if (arg == "--output") {
                options.output = value;
            } else {
                if (value != "auto" && value != "bitcode" &&
                    value != "source") {
                    error = "unknown --input-type '" + value +
                            "' (expected auto, bitcode or source)";
                    return false;
                }
                options.inputType = value;
            }
        } else if (arg == "--include-library") {
            options.includeLibrary = true;
        } else if (arg == "--all" || arg == "-a") {
            options.allHeaders = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg.rfind("--", 0) == 0) {
            error = "unknown option: " + arg;
            return false;
        } else {
            options.inputs.push_back(arg);
        }
    }
    if (options.inputs.empty()) {
        error = "no input file given";
        return false;
    }
    return true;
}

/// Resolve the backend for all inputs, rejecting mixed input kinds.
bool resolveInputType(const ClassOptions& options, InputType& type,
                      std::string& error) {
    if (options.inputType == "bitcode") {
        type = InputType::Bitcode;
        return true;
    }
    if (options.inputType == "source") {
        type = InputType::Source;
        return true;
    }

    type = InputType::Unknown;
    for (const std::string& input : options.inputs) {
        const InputType detected = detectInputType(input);
        if (detected == InputType::Unknown) {
            std::ifstream probe(input, std::ios::binary);
            if (!probe)
                error = "cannot open input file: " + input;
            else
                error = "cannot determine the type of '" + input +
                        "'; use --input-type bitcode|source";
            return false;
        }
        if (type != InputType::Unknown && type != detected) {
            error = "mixed input types: all inputs must be either bitcode or "
                    "C/C++ source";
            return false;
        }
        type = detected;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    ClassOptions options;
    std::string error;
    if (!parseArgs(argc, argv, options, error)) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        printUsage(argv[0]);
        return 1;
    }

    InputType type = InputType::Unknown;
    if (!resolveInputType(options, type, error)) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        return 1;
    }
    if (type == InputType::Bitcode && options.inputs.size() > 1) {
        std::fprintf(stderr,
                     "[ERROR] only one bitcode file can be analyzed at a "
                     "time\n");
        return 1;
    }

    if (type == InputType::Bitcode &&
        (options.allHeaders || !options.clangArgs.empty()))
        std::fprintf(stderr,
                     "[WARN] --all and -- <clang args> only apply to source "
                     "inputs; ignored\n");
    if (type == InputType::Source && options.includeLibrary)
        std::fprintf(stderr,
                     "[WARN] --include-library only applies to bitcode "
                     "inputs; ignored\n");

    ClassReport report;
    report.inputs = options.inputs;
    report.inputType = type;

    const bool ok = type == InputType::Bitcode
                        ? extractClassesFromBitcode(options, report, error)
                        : extractClassesFromSource(options, report, error);
    if (!ok) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        return 1;
    }

    if (type == InputType::Bitcode && !report.hasDebugInfo)
        std::fprintf(stderr,
                     "[WARN] %s carries no DWARF debug info; class "
                     "information is only\n"
                     "       present when the sources are compiled with -g.\n"
                     "       Nothing to extract -- writing an empty report.\n",
                     options.inputs.front().c_str());

    if (!writeClassReport(report, options, error)) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        return 1;
    }

    size_t methodCount = 0, edgeCount = 0;
    for (const ClassInfo& cls : report.classes) {
        methodCount += cls.methods.size();
        edgeCount += cls.bases.size();
    }
    std::fprintf(stderr,
                 "[class_extractor] input_type=%s extractor=%s classes=%zu "
                 "methods=%zu inheritance_edges=%zu",
                 inputTypeName(type), extractorName(type),
                 report.classes.size(), methodCount, edgeCount);
    if (report.hiddenClasses > 0)
        std::fprintf(stderr, " hidden_library_classes=%zu",
                     report.hiddenClasses);
    if (type == InputType::Bitcode && !report.hasDebugInfo)
        std::fprintf(stderr, " no_debug_info=1");
    if (options.includeLibrary && type == InputType::Bitcode)
        std::fprintf(stderr, " (library classes included)");
    std::fprintf(stderr, "\n");
    return 0;
}
