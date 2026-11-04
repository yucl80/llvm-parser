#ifndef CALLGRAPH_OUTPUT_H
#define CALLGRAPH_OUTPUT_H

#include "callgraph_analysis.h"
#include "function_filter.h"

#include <llvm/IR/Module.h>

namespace SVF {
class CallGraph;
}

/// Write the call-graph report per config.format:
///  - ReportFormat::Tree (default): the metadata header, then one
///    ===== entry ===== block per configured entry rendering the call chain
///    as an indented ASCII tree;
///  - ReportFormat::Tsv: a #-prefixed metadata header followed by one
///    #entry/#end block per configured entry, each holding the call tree as
///    tab-separated span lines.
///
/// Span lines are streamed to \p config.outputFile (or
/// "<bitcode-stem>.callgraph.tsv" / ".callgraph.tree") as the tree is
/// traversed, so the TSV report is never materialized in memory; the tree
/// layout buffers one entry's spans to place the connectors. \p filter is the
/// pre-analysis classification used to hide library code under excludeStd and
/// the functions named by the user's filter file (config.nameFilters); hidden
/// functions report their direct user callees but not their internal
/// sub-call tree.
void writeCallGraphReport(const llvm::Module& M,
                          const AnalysisConfig& config,
                          SVF::CallGraph* callGraph,
                          const FunctionFilter& filter);

#endif // CALLGRAPH_OUTPUT_H
