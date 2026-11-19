// CLI driver; individual scanners are registered by the bug-analyzer plugins.
#include "bug_analyzer_plugin.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Options {
    std::string format = "text";
    std::string output;
    std::vector<std::string> inputs;
};

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--format text|json] [--output file] "
                 "<bitcode.bc|llvm-ir.ll>...\n"
                 "\n"
                 "Scans LLVM modules for heuristic bug candidates. "
                 "Results require manual validation.\n",
                 program);
}

bool parseArgs(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--format" || arg == "--output") {
            if (++i >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            const std::string value = argv[i];
            if (arg == "--format") {
                if (value != "text" && value != "json") {
                    error = "unknown format '" + value +
                            "' (expected text or json)";
                    return false;
                }
                options.format = value;
            } else {
                options.output = value;
            }
        } else if (arg.rfind("--", 0) == 0) {
            error = "unknown option: " + arg;
            return false;
        } else {
            options.inputs.push_back(arg);
        }
    }
    if (options.inputs.empty()) {
        error = "no bitcode or LLVM IR input given";
        return false;
    }
    return true;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                result += escaped;
            } else {
                result += static_cast<char>(ch);
            }
        }
    }
    return result;
}

void writeJson(std::ostream& output,
               const std::vector<std::string>& inputs,
               const std::vector<buganalyzer::Finding>& findings) {
    output << "{\"analysis\":\"heuristic candidates; manual validation "
              "required\",\"inputs\":[";
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (i)
            output << ",";
        output << "\"" << jsonEscape(inputs[i]) << "\"";
    }
    output << "],\"findings\":[";
    for (std::size_t i = 0; i < findings.size(); ++i) {
        const buganalyzer::Finding& finding = findings[i];
        if (i)
            output << ",";
        output << "{\"input\":\"" << jsonEscape(finding.input)
               << "\",\"rule\":\"" << jsonEscape(finding.rule)
               << "\",\"cwe\":\"" << jsonEscape(finding.cwe)
               << "\",\"severity\":\"" << jsonEscape(finding.severity)
               << "\",\"category\":\"" << jsonEscape(finding.category)
               << "\",\"message\":\"" << jsonEscape(finding.message)
               << "\",\"function\":\"" << jsonEscape(finding.function)
               << "\",\"file\":\"" << jsonEscape(finding.file)
               << "\",\"line\":" << finding.line
               << ",\"start_line\":" << finding.startLine
               << ",\"end_line\":" << finding.endLine
               << ",\"column\":" << finding.column << "}";
    }
    output << "]}\n";
}

void writeText(std::ostream& output,
               const std::vector<buganalyzer::Finding>& findings) {
    if (findings.empty()) {
        output << "No supported vulnerability patterns found.\n";
        return;
    }
    for (const buganalyzer::Finding& finding : findings) {
        output << (finding.file.empty() ? finding.input : finding.file) << ":"
               << finding.startLine << "-" << finding.endLine << ":"
               << finding.column << ": "
               << finding.severity << " [" << finding.rule << ", "
               << finding.cwe << "] " << finding.function << ": "
               << finding.message << "\n";
    }
    output << "Note: findings are heuristic candidates and require manual "
              "validation.\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parseArgs(argc, argv, options, error)) {
        std::fprintf(stderr, "[ERROR] %s\n", error.c_str());
        printUsage(argv[0]);
        return 1;
    }

    std::vector<buganalyzer::Finding> findings;
    for (const std::string& input : options.inputs) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> module =
            llvm::parseIRFile(input, diagnostic, context);
        if (!module) {
            std::string message;
            llvm::raw_string_ostream stream(message);
            diagnostic.print(argv[0], stream);
            stream.flush();
            std::fprintf(stderr, "[ERROR] %s\n", message.c_str());
            return 1;
        }
        buganalyzer::runAnalyzers(*module, input, findings);
    }

    std::sort(findings.begin(), findings.end(),
              [](const buganalyzer::Finding& left,
                 const buganalyzer::Finding& right) {
                  return std::tie(left.file, left.line, left.column,
                                  left.function, left.rule, left.input) <
                         std::tie(right.file, right.line, right.column,
                                  right.function, right.rule, right.input);
              });

    std::ofstream file;
    std::ostream* output = &std::cout;
    if (!options.output.empty()) {
        file.open(options.output);
        if (!file) {
            std::fprintf(stderr, "[ERROR] cannot write output file: %s\n",
                         options.output.c_str());
            return 1;
        }
        output = &file;
    }

    if (options.format == "json")
        writeJson(*output, options.inputs, findings);
    else
        writeText(*output, findings);
    if (!*output) {
        std::fprintf(stderr, "[ERROR] failed while writing report\n");
        return 1;
    }
    return 0;
}
