#include "callgraph_output.h"

#include "callgraph_tsv_sink.h"
#include "callgraph_tree_sink.h"

#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace SVF;

/// Demangle a C++ function name for human-readable output.
///
/// Results are memoized per mangled name: the output call graph re-visits the
/// same function on many paths (it is a call tree), so demangling it afresh
/// on every span is the dominant serialization cost on large projects. The
/// cache is thread-local so the traversal can be parallelized later.
///
/// Returns a reference into the cache (element references survive unordered_map
/// rehash), so a call-tree span referencing the same function does not copy the
/// demangled string on every visit.
static const std::string& demangleFuncName(const std::string& mangled) {
    static thread_local std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(mangled);
    if (it != cache.end()) return it->second;
    std::string result = llvm::demangle(mangled);
    if (result.empty()) result = mangled;
    return cache.emplace(mangled, std::move(result)).first->second;
}

/// Minimum source location of the call site(s) backing an edge, cached per
/// edge. Each call site is an LLVM call instruction, recovered from the edge's
/// CallICFGNode via LLVMModuleSet's reverse value map. The outermost debug
/// location is used so inlined call text still reports the caller's location.
///
/// Only the minimum line is needed (the caller uses front() of the previously
/// sorted set), and an edge is re-traversed for every path that reaches it, so
/// the result is computed once per unique edge and memoized.
static std::optional<CallSiteLoc> getMinCallSiteLoc(
    const CallGraphEdge* edge,
    LLVMModuleSet* modSet,
    std::unordered_map<const CallGraphEdge*, std::optional<CallSiteLoc>>& cache) {
    auto it = cache.find(edge);
    if (it != cache.end()) return it->second;

    std::optional<CallSiteLoc> minLoc;
    auto collect = [&](CallGraphEdge::CallInstSet::const_iterator begin,
                       CallGraphEdge::CallInstSet::const_iterator end) {
        for (auto vit = begin; vit != end; ++vit) {
            if (!modSet->hasLLVMValue(*vit)) continue;
            const auto* call = llvm::dyn_cast_or_null<const llvm::CallBase>(
                modSet->getLLVMValue(*vit));
            const llvm::DILocation* loc = call ? call->getDebugLoc() : nullptr;
            if (!loc) continue;
            while (loc->getInlinedAt()) loc = loc->getInlinedAt();
            if (!minLoc || loc->getLine() < minLoc->line)
                minLoc = CallSiteLoc{loc->getFilename().str(), loc->getLine()};
        }
    };
    collect(edge->directCallsBegin(), edge->directCallsEnd());
    collect(edge->indirectCallsBegin(), edge->indirectCallsEnd());
    cache.emplace(edge, minLoc);
    return minLoc;
}

/// Lazy per-function descriptor cache, keyed by the function's FunObjVar
/// (unique per function; the name string is only materialized on a miss).
using FuncInfoCache = std::unordered_map<const FunObjVar*, FuncInfo>;

/// Compute (or fetch) the descriptor for a function. isStd is only filled when
/// excludeStd is on — otherwise nothing is excluded and the flag stays false.
/// The user's name filters apply unconditionally, so a filtered function is
/// hidden even with --include-stdlib. isApp marks functions defined in the
/// application's own sources: they stay visible even when reached through a
/// system-header call site.
static const FuncInfo& getFuncInfo(const FunObjVar* func,
                                   bool excludeStd,
                                   const FunctionFilter& filter,
                                   FuncInfoCache& cache) {
    auto it = cache.find(func);
    if (it != cache.end()) return it->second;
    FuncInfo info;
    info.mangled = func->getName();
    const std::string& demangled = demangleFuncName(info.mangled);
    info.function = stripParams(demangled);
    if (const FuncLineRange* range = filter.findLineRange(info.mangled);
        range && range->hasInfo) {
        info.file = range->file;
        info.start = range->start;
        info.end = range->end;
        info.hasLines = true;
    }
    if (excludeStd)
        info.isStd = filter.isExcluded(info.mangled, demangled);
    info.filtered = filter.isFiltered(info.mangled, demangled);
    info.isApp = filter.isAppFunction(info.mangled);
    return cache.emplace(func, std::move(info)).first->second;
}

/// Recursively stream spans for the call graph rooted at \p node, mirroring the
/// tree traversal (one span per call edge, cycle-cut). Each span is handed to
/// \p sink as it is generated — the TSV sink writes it out immediately, the
/// tree sink buffers it — so no format ever materializes the whole report
/// unless it must; \p spanCount is the running span-id counter.
/// \p reachable collects the set of unique function names reachable from the
/// entry (for the per-entry summary line).
///
/// A node is hidden — no span written — when it is library code under
/// \p excludeStd, matches a user name filter (--filter-file), or is reached
/// through a system-header call site (\p suppressSpan, a library callee only).
/// Hiding follows three rules:
///  1. an edge from a user function into a hidden function is not reported;
///  2. an edge from a hidden function into user code IS reported: the callee is
///     grafted onto the nearest emitted ancestor (parent span / depth pass
///     through unchanged), so a callback invoked from inside library machinery
///     still shows up without the library's span;
///  3. an edge from a hidden function into another hidden function is not
///     followed at all — the library's internal sub-call tree is neither
///     displayed nor traversed.
/// Hidden nodes still join \p in_stack so recursion is cycle-cut.
static void emitSpans(CallGraphNode* node,
                      unsigned depth,
                      const std::optional<unsigned>& parentSpanId,
                      bool indirect,
                      const std::optional<CallSiteLoc>& callLoc,
                      bool suppressSpan,
                      std::set<CallGraphNode*>& in_stack,
                      const FunctionFilter& filter,
                      LLVMModuleSet* modSet,
                      std::unordered_map<const CallGraphEdge*, std::optional<CallSiteLoc>>& callSiteLocCache,
                      FuncInfoCache& infoCache,
                      SpanSink& sink,
                      unsigned& spanCount,
                      std::unordered_set<std::string>& reachable,
                      bool excludeStd) {
    const FunObjVar* func = node->getFunction();
    if (!func || in_stack.count(node) || func->isDeclaration()) return;

    const FuncInfo& info = getFuncInfo(func, excludeStd, filter, infoCache);

    // Hidden = library code, filter match, or a library callee reached from a
    // system-header call site. A user function is never hidden by the call
    // site alone, so "library calls user callback" survives rule 1.
    const bool hidden = info.isStd || info.filtered ||
                        (suppressSpan && !info.isApp);
    std::optional<unsigned> mySpanId;
    if (!hidden) {
        mySpanId = spanCount++;
        sink.span(info, *mySpanId, parentSpanId, indirect, depth, false, callLoc);
        reachable.insert(info.mangled);
    }

    if (node->OutEdgeBegin() == node->OutEdgeEnd()) return;

    in_stack.insert(node);

    // Depth / parent for the next emitted node: bump only past emitted nodes.
    const std::optional<unsigned> childParent = hidden ? parentSpanId : mySpanId;
    const unsigned childDepth = hidden ? depth : depth + 1;

    for (auto eit = node->OutEdgeBegin(); eit != node->OutEdgeEnd(); ++eit) {
        CallGraphEdge* edge = *eit;
        CallGraphNode* callee = edge->getDstNode();
        if (!callee->getFunction()) continue;

        const bool childIndirect = edge->isIndirectCallEdge();
        std::optional<CallSiteLoc> cl = getMinCallSiteLoc(edge, modSet, callSiteLocCache);

        const FuncInfo& ci = getFuncInfo(callee->getFunction(), excludeStd,
                                         filter, infoCache);
        // Release-build fallback (no debug info): a call made inside a system
        // include header marks the callee as library code — but never hides a
        // user function, whose span is required by rule 2.
        const bool calleeSuppress =
            cl && isSystemHeaderFile(cl->file) && !ci.isApp;
        const bool calleeHidden = ci.isStd || ci.filtered || calleeSuppress;

        // Rule 3: no library-internal sub-call tree. From a hidden node only
        // edges into user code are followed; hidden -> hidden edges produce no
        // span and are not traversed. A visible node still descends into a
        // hidden callee, which is how the callbacks it invokes are reported.
        if (hidden && calleeHidden) continue;

        if (in_stack.count(callee)) {
            // Cycle back-edge: record the span but do not expand its subtree.
            if (calleeHidden) continue;
            sink.span(ci, spanCount++, childParent, childIndirect, childDepth,
                      true, cl);
            reachable.insert(ci.mangled);
            continue;
        }

        emitSpans(callee, childDepth, childParent, childIndirect, cl,
                  calleeSuppress, in_stack, filter, modSet, callSiteLocCache,
                  infoCache, sink, spanCount, reachable, excludeStd);
    }
    in_stack.erase(node);
}

/// Write the #-prefixed metadata header. Per-entry span lines are streamed
/// during the traversal, so the report is never materialized in memory.
static void writeHeader(std::ostream& os,
                        const llvm::Module& M,
                        const AnalysisConfig& config,
                        const std::vector<std::string>& entryNames) {
    os << "#schema_version 2\n";
    os << "#bitcode " << tsvEscape(M.getModuleIdentifier()) << "\n";
    os << "#analysis pta=";
    if (config.useFlowSensitive)
        os << "FlowSensitive";
    else if (config.useVersionedFS)
        os << "VersionedFlowSensitive";
    else
        os << "AndersenWaveDiff";
    os << " context_sensitive=" << (config.contextSensitive ? 1 : 0)
       << " heap_model=" << (config.heapModel ? 1 : 0)
       << " exclude_std=" << (config.excludeStd ? 1 : 0) << "\n";
    if (!config.filterFile.empty())
        os << "#filter_file " << tsvEscape(config.filterFile)
           << " patterns=" << config.nameFilters.size() << "\n";
    os << "#entries";
    for (const std::string& name : entryNames)
        os << " " << tsvEscape(name);
    os << "\n";
}

/// Resolve an entry name (mangled, demangled, or param-stripped) to its
/// call-graph node. Exact matches win; a param-stripped name matching multiple
/// overloads warns and picks the first.
static CallGraphNode* resolveEntry(CallGraph* cg, const std::string& name) {
    std::vector<CallGraphNode*> baseMatches;
    for (auto it = cg->begin(); it != cg->end(); ++it) {
        CallGraphNode* node = it->second;
        const FunObjVar* func = node->getFunction();
        if (!func) continue;
        const std::string& mangled = func->getName();
        std::string demangled = demangleFuncName(mangled);
        if (mangled == name || demangled == name)
            return node;
        if (stripParams(demangled) == name)
            baseMatches.push_back(node);
    }
    if (baseMatches.size() > 1) {
        llvm::errs() << "[WARN] ambiguous entry \"" << name << "\" matches "
                     << baseMatches.size() << " overloads; use a mangled name for one of:";
        for (CallGraphNode* n : baseMatches)
            llvm::errs() << " " << n->getFunction()->getName();
        llvm::errs() << "\n";
    }
    return baseMatches.empty() ? nullptr : baseMatches.front();
}

/// Default report path: "<bitcode-stem>.callgraph.tsv", or
/// "<bitcode-stem>.callgraph.tree" for the tree layout.
static std::string defaultOutputPath(const llvm::Module& M, bool treeFormat) {
    std::string id = M.getModuleIdentifier();
    size_t dot = id.rfind('.');
    if (!id.empty() && dot != std::string::npos)
        id = id.substr(0, dot);
    return (id.empty() ? "callgraph" : id) +
           (treeFormat ? ".callgraph.tree" : ".callgraph.tsv");
}

void writeCallGraphReport(const llvm::Module& M,
                          const AnalysisConfig& config,
                          CallGraph* callGraph,
                          const FunctionFilter& filter) {
    // Determine entries (default: main).
    std::vector<std::string> entryNames = config.entries;
    if (entryNames.empty())
        entryNames.push_back("main");

    // Stream the report to a file: header, then one entry block per resolved
    // entry with its spans written as they are generated. Peak memory is
    // O(depth + unique functions + unique edges) for the TSV layout, not
    // O(span count), so multi-million-span trees no longer balloon; the tree
    // layout additionally buffers one entry's spans to place the connectors.
    // The reverse value map and the per-edge call-site-location cache are
    // shared across entries (edges are stable objects), so call locations are
    // extracted once.
    const bool treeFormat = config.format == AnalysisConfig::ReportFormat::Tree;
    std::string outPath = config.outputFile.empty()
        ? defaultOutputPath(M, treeFormat)
        : config.outputFile;
    static char outBuf[4 << 20];
    std::filebuf fb;
    fb.pubsetbuf(outBuf, sizeof(outBuf));
    if (!fb.open(outPath.c_str(), std::ios::out)) {
        llvm::errs() << "[ERROR] cannot open output file: " << outPath << "\n";
        return;
    }

    std::ostream os(&fb);
    TsvSink tsvSink(os);
    TreeSink treeSink(os);
    SpanSink& sink = treeFormat ? static_cast<SpanSink&>(treeSink)
                                : static_cast<SpanSink&>(tsvSink);
    LLVMModuleSet* modSet = LLVMModuleSet::getLLVMModuleSet();
    std::unordered_map<const CallGraphEdge*, std::optional<CallSiteLoc>> callSiteLocCache;
    FuncInfoCache infoCache;
    infoCache.reserve(M.size());  // upper bound on unique functions
    writeHeader(os, M, config, entryNames);
    for (const std::string& name : entryNames) {
        CallGraphNode* entryNode = resolveEntry(callGraph, name);
        if (!entryNode) {
            llvm::errs() << "[WARN] entry function not found: " << name << "\n";
            continue;
        }
        if (getFuncInfo(entryNode->getFunction(), config.excludeStd, filter,
                        infoCache)
                .filtered) {
            llvm::errs() << "[WARN] entry function matches a filter pattern, "
                            "skipped: " << name << "\n";
            continue;
        }
        sink.beginEntry(name);
        unsigned spanCount = 0;
        std::set<CallGraphNode*> in_stack;
        std::unordered_set<std::string> reachable;
        reachable.reserve(M.size());
        emitSpans(entryNode, 0, std::nullopt, /*indirect=*/false, std::nullopt,
                  false, in_stack, filter, modSet, callSiteLocCache, infoCache,
                  sink, spanCount, reachable, config.excludeStd);
        sink.endEntry(name, spanCount, reachable.size());
    }
    os.flush();
    if (os.good() && fb.close())
        llvm::outs() << "report written to " << outPath << "\n";
    else
        llvm::errs() << "[ERROR] failed to write output file: " << outPath << "\n";
}
