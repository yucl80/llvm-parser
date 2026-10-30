#ifndef CALLGRAPH_OUTPUT_H
#define CALLGRAPH_OUTPUT_H

#include "callgraph_analysis.h"
#include "function_filter.h"

#include <llvm/IR/Module.h>

namespace SVF {
class CallGraph;
}

/// Write the call-graph report: a #-prefixed metadata header followed by one
/// #entry/#end block per configured entry, each holding the call tree as
/// tab-separated span lines.
///
/// Span lines are streamed to \p config.outputFile (or
/// "<bitcode-stem>.callgraph.tsv") as the tree is traversed, so the report is
/// never materialized in memory. \p filter is the pre-analysis classification
/// used to hide library code under excludeStd.
void writeCallGraphReport(const llvm::Module& M,
                          const AnalysisConfig& config,
                          SVF::CallGraph* callGraph,
                          const FunctionFilter& filter);

#endif // CALLGRAPH_OUTPUT_H
