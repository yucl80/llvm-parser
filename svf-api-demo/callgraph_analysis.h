#ifndef CALLGRAPH_ANALYSIS_H
#define CALLGRAPH_ANALYSIS_H

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

/// Configuration for the pointer analysis.
struct AnalysisConfig {
    bool useFlowSensitive = false;
    bool useVersionedFS = false;
    bool contextSensitive = false;
    bool heapModel = false;

    /// Call-graph entry functions (demangled or mangled names).
    /// Empty => default entry "main".
    std::vector<std::string> entries;

    /// Output file path; empty => "<bitcode-stem>.callgraph.json".
    std::string outputFile;
};

/// Run call graph analysis on an LLVM module and write the per-entry
/// call graphs (spans) as JSON to a file.
void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config);

#endif // CALLGRAPH_ANALYSIS_H
