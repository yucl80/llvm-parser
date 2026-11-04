#ifndef CALLGRAPH_SPAN_H
#define CALLGRAPH_SPAN_H

#include <optional>
#include <string>

/// Shared data types carried from the call-graph traversal to the report
/// sinks. Kept free of SVF/LLVM dependencies so the sink layer only deals
/// with plain output records.

/// Source location of a call site: the file and line where the call text
/// appears in the analyzed source. An edge is often backed by several call
/// instructions (a loop, or the same helper reached from multiple sites); the
/// minimum line is the representative one, and its file is carried along.
struct CallSiteLoc {
    std::string file;
    unsigned line = 0;
};

/// Precomputed, per-function output fields. The output is a call tree, so the
/// same function is re-visited on many paths; everything derivable from the
/// function — demangled name, source location, std-exclusion — is computed
/// once per unique function and reused across all its spans.
struct FuncInfo {
    std::string mangled;    ///< raw symbol name
    std::string function;   ///< demangled name, params stripped
    std::string file;
    unsigned start = 0;
    unsigned end = 0;
    bool hasLines = false;
    bool isStd = false;     ///< library code: hidden from output when excludeStd is on
    bool filtered = false;  ///< matches a --filter-file pattern: hidden from output
    bool isApp = false;     ///< defined in the application's own source tree
};

#endif // CALLGRAPH_SPAN_H
