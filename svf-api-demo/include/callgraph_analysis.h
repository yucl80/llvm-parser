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
    /// Hide library code from the output call graph so only the analyzed
    /// application's own call chains are reported. A function is library code
    /// when the file it is *defined* in is outside the application's source
    /// tree (derived from the module's compile units) — this covers the C
    /// library (strrchr, memcpy, ...), libstdc++/glibc headers (std::,
    /// __gthread_mutex_lock, ...) and any other linked-in library source,
    /// without enumerating function names. In addition:
    ///  - symbols with no definition in the module are imported code;
    ///  - the std::/__gnu_cxx:: namespaces and the C++ runtime ABI (__cxa_*,
    ///    __gxx_*, _Unwind_*, operator new/delete) are matched by symbol;
    ///  - calls made from inside a system header are dropped.
    /// The symbol and body checks also work on release builds without -g, where
    /// the definition file is unavailable. User callbacks invoked from inside
    /// library functions (comparators, lambdas, functors) are still shown,
    /// grafted onto the caller that reached them.
    bool excludeStd = true;

    /// Call-graph entry functions (demangled or mangled names).
    /// Empty => default entry "main".
    std::vector<std::string> entries;

    /// Output file path; empty => "<bitcode-stem>.callgraph.tsv".
    std::string outputFile;
};

/// Load the bitcode file at \p bcFile and run call graph analysis on it,
/// writing the per-entry call graphs (spans) to a file.
/// Returns 0 on success, non-zero if the bitcode cannot be loaded.
int analyzeCallGraph(const std::string& bcFile, const AnalysisConfig& config);

#endif // CALLGRAPH_ANALYSIS_H
