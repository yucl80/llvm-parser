#ifndef CALLGRAPH_ANALYSIS_H
#define CALLGRAPH_ANALYSIS_H

#include <llvm/IR/Module.h>

/// Configuration for the pointer analysis.
struct AnalysisConfig {
    bool useFlowSensitive = false;
    bool useVersionedFS = false;
    bool contextSensitive = false;
    bool heapModel = false;
};

/// Run call graph analysis on an LLVM module and print the call tree.
void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config);

#endif // CALLGRAPH_ANALYSIS_H
