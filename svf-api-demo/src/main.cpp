#include "callgraph_analysis.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static void printUsage(const char* prog) {
    llvm::errs() << "Usage: " << prog << " [options] <bitcode-file>\n"
                 << "Options:\n"
                 << "  (default)          Flow-sensitive + context-sensitive\n"
                 << "                     (most complete & precise call graph)\n"
                 << "  --fs               Use flow-sensitive analysis (FSSPARSE_WPA) [default on]\n"
                 << "  --vfs              Use versioned flow-sensitive analysis (VFS_WPA)\n"
                 << "  --ander            Downgrade to flow-insensitive AndersenWaveDiff\n"
                 << "  --dda              Demand-driven value-flow analysis (ContextDDA):\n"
                 << "                     only the function-pointer / vtable-pointer\n"
                 << "                     variables feeding indirect callsites are\n"
                 << "                     solved on demand on the SVFG — much faster\n"
                 << "                     than a full points-to solve for call-graph\n"
                 << "                     construction [default: off]\n"
                 << "  --dda-flow         Demand-driven with FlowDDA (flow-sensitive\n"
                 << "                     instead of context-sensitive on-demand solve;\n"
                 << "                     implies --dda)\n"
                 << "  --dda-cxt-len <n>  DDA: maximum calling-context length\n"
                 << "                     (SVF MaxContextLen, default 3; larger = more\n"
                 << "                     precise, slower)\n"
                 << "  --dda-opt-svfg     DDA: build the optimised pointer-only SVFG\n"
                 << "                     (SVFGOPT — merges context-insensitive regions,\n"
                 << "                     drops formal-in/actual-out): smaller graph,\n"
                 << "                     faster on-demand traversal, some precision loss\n"
                 << "  --cs               Context sensitivity [default on; SVF SVFG is\n"
                 << "                     context-sensitive by default]\n"
                 << "  --heap-model       Heap object model (ModelConsts + ModelArrays)\n"
                 << "                     [opt-in; resolves local fn-ptr arrays but can\n"
                 << "                      under-resolve virtual/struct dispatch]\n"
                 << "  --include-stdlib   Include library code in the call graph\n"
                 << "                     (functions defined outside the project's\n"
                 << "                     own sources: C library, std::/__gnu_cxx::,\n"
                 << "                     __cxa_*/__gxx_* runtime ABI)\n"
                 << "                     [default: excluded]\n"
                 << "  --entry <name>     Call-graph entry function (repeatable; demangled or mangled)\n"
                 << "  --entry-file <f>   File listing entry names, one per line\n"
                 << "  --filter-file <f>  Hide functions matching the regular expressions in\n"
                 << "                     <f> (one ECMAScript regex per line; blank lines\n"
                 << "                     and '#' comments are skipped). Matched functions are\n"
                 << "                     hidden like library code: the user functions they\n"
                 << "                     call are still reported, their own callees are not\n"
                 << "  --format <tsv|tree>\n"
                 << "                     Report layout: an indented call tree per entry\n"
                 << "                     with [indirect]/[cycle] markers and call-site\n"
                 << "                     locations (default), or tab-separated spans\n"
                 << "  --output <file>    Write the call-graph report to <file>\n"
                 << "                     (default <bitcode>.callgraph.tree, or\n"
                 << "                     <bitcode>.callgraph.tsv with --format tsv)\n";
}

/// Trim leading/trailing blanks and tabs, so indented patterns and stray
/// trailing whitespace in a filter file do not change what is matched.
static std::string trim(const std::string& s) {
    const char* kBlanks = " \t";
    size_t begin = s.find_first_not_of(kBlanks);
    if (begin == std::string::npos) return {};
    size_t end = s.find_last_not_of(kBlanks);
    return s.substr(begin, end - begin + 1);
}

/// Read a line-oriented configuration file: one regex per line, blank lines and
/// '#' comments skipped. Lines are trimmed; CRLF endings are accepted.
/// Returns false when the file cannot be opened.
static bool readPatternFile(const std::string& path,
                            std::vector<std::string>& patterns) {
    std::ifstream in(path);
    if (!in) {
        llvm::errs() << "[ERROR] cannot open filter file: " << path << "\n";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        patterns.push_back(std::move(line));
    }
    return true;
}

static AnalysisConfig parseFlags(int argc, char** argv, int& bcIdx) {
    AnalysisConfig config;
    bcIdx = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fs") == 0) {
            config.useFlowSensitive = true;
            config.useVersionedFS = false;
        } else if (strcmp(argv[i], "--vfs") == 0) {
            config.useVersionedFS = true;
            config.useFlowSensitive = false;
        } else if (strcmp(argv[i], "--ander") == 0) {
            // Downgrade to flow-insensitive AndersenWaveDiff (faster, less precise).
            config.useFlowSensitive = false;
            config.useVersionedFS = false;
            config.useDDA = false;
        } else if (strcmp(argv[i], "--dda") == 0) {
            // Demand-driven value-flow analysis: solve only the pointers
            // feeding indirect callsites (fastest precise mode).
            config.useDDA = true;
            config.ddaFlowSensitive = false;
            config.useFlowSensitive = false;
            config.useVersionedFS = false;
        } else if (strcmp(argv[i], "--dda-flow") == 0) {
            config.useDDA = true;
            config.ddaFlowSensitive = true;
            config.useFlowSensitive = false;
            config.useVersionedFS = false;
        } else if (strcmp(argv[i], "--dda-cxt-len") == 0 && i + 1 < argc) {
            config.ddaContextLen =
                static_cast<unsigned>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--dda-opt-svfg") == 0) {
            config.ddaOptSvfg = true;
        } else if (strcmp(argv[i], "--cs") == 0) {
            config.contextSensitive = true;
        } else if (strcmp(argv[i], "--heap-model") == 0) {
            config.heapModel = true;
        } else if (strcmp(argv[i], "--no-heap-model") == 0) {
            config.heapModel = false;
        } else if (strcmp(argv[i], "--include-stdlib") == 0) {
            config.excludeStd = false;
        } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
            config.entries.push_back(argv[++i]);
        } else if (strcmp(argv[i], "--entry-file") == 0 && i + 1 < argc) {
            std::ifstream ef(argv[++i]);
            if (!ef) {
                llvm::errs() << "Cannot open entry file: " << argv[i] << "\n";
                continue;
            }
            std::string line;
            while (std::getline(ef, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    config.entries.push_back(line);
            }
        } else if (strcmp(argv[i], "--filter-file") == 0 && i + 1 < argc) {
            // A missing filter file is not fatal: the analysis runs unfiltered
            // rather than silently hiding nothing while claiming a filter.
            const std::string path = argv[++i];
            if (readPatternFile(path, config.nameFilters))
                config.filterFile = path;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            const char* fmt = argv[++i];
            if (strcmp(fmt, "tsv") == 0) {
                config.format = AnalysisConfig::ReportFormat::Tsv;
            } else if (strcmp(fmt, "tree") == 0) {
                config.format = AnalysisConfig::ReportFormat::Tree;
            } else {
                llvm::errs() << "[ERROR] unknown --format '" << fmt
                             << "' (expected tsv or tree)\n";
                exit(1);
            }
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config.outputFile = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else {
            // Assume it's the bitcode file path
            bcIdx = i;
        }
    }

    return config;
}

int main(int argc, char** argv) {
    int bcIdx;
    AnalysisConfig config = parseFlags(argc, argv, bcIdx);

    if (bcIdx == -1) {
        llvm::errs() << "[ERROR] bitcode file path is required\n";
        exit(1);
    }    

    return analyzeCallGraph(argv[bcIdx] , config);
}
