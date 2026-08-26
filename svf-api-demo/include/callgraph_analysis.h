#ifndef CALLGRAPH_ANALYSIS_H
#define CALLGRAPH_ANALYSIS_H

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

/// Configuration for the pointer analysis.
/// Defaults are tuned for the most complete + precise call graphs:
/// flow-sensitive + context-sensitive SVFG; heap-object model is opt-in;
/// C++ standard-library functions are excluded from the output.
struct AnalysisConfig {
    bool useFlowSensitive = true;
    bool useVersionedFS = false;
    bool contextSensitive = true;
    bool heapModel = false;
    /// Hide library code from the output call graph: C++ standard library /
    /// runtime functions (std::, __gnu_cxx::, __cxa_* exception ABI, operator
    /// new/delete) and any function defined in a system include header
    /// (libstdc++/glibc templates etc., e.g. under /usr/include). Name-based
    /// filtering alone misses many libstdc++ functions whose demangled names
    /// carry a return-type prefix ("void std::..."), so file location is used
    /// as the reliable signal. User callbacks invoked from inside library
    /// functions (comparators, lambdas, functors) are still shown, grafted
    /// onto the caller that reached them.
    bool excludeStd = true;

    /// Call-graph entry functions (demangled or mangled names).
    /// Empty => default entry "main".
    std::vector<std::string> entries;

    /// Output file path; empty => "<bitcode-stem>.callgraph.tsv".
    std::string outputFile;
};

/// Run call graph analysis on an LLVM module and write the per-entry
/// call graphs (spans) as JSON to a file.
void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config);

#endif // CALLGRAPH_ANALYSIS_H
