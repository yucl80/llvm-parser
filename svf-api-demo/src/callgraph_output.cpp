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
#include <string>
#include <string_view>
#include <unordered_map>
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
static const std::optional<CallSiteLoc>& getMinCallSiteLoc(
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
    return cache.emplace(edge, std::move(minLoc)).first->second;
}

/// Lazy per-function descriptor cache, keyed by the function's FunObjVar
/// (unique per function; the name string is only materialized on a miss).
using FuncInfoCache = std::unordered_map<const FunObjVar*, FuncInfo>;

/// Intern mangled names to dense IDs. The report's `reachable_funcs` count is
/// defined over unique names, not unique FunObjVars, so two same-named
/// definitions from different modules must still count once. Keys are views of
/// SVF-owned names, which outlive this per-report interner, so no extra string
/// copies are stored here.
struct NameInterner {
    std::unordered_map<std::string_view, unsigned> ids;

    unsigned get(const FunObjVar* func) {
        std::string_view name = func->getName();
        auto it = ids.find(name);
        if (it != ids.end()) return it->second;
        const unsigned id = static_cast<unsigned>(ids.size());
        ids.emplace(name, id);
        return id;
    }
};

/// Compute (or fetch) the descriptor for a function. isStd is only filled when
/// excludeStd is on — otherwise nothing is excluded and the flag stays false.
/// The user's name filters apply unconditionally, so a filtered function is
/// hidden even with --include-stdlib. isApp marks functions defined in the
/// application's own sources: they stay visible even when reached through a
/// system-header call site.
static const FuncInfo& getFuncInfo(const FunObjVar* func,
                                   bool excludeStd,
                                   const FunctionFilter& filter,
                                   FuncInfoCache& cache,
                                   NameInterner& interner) {
    auto it = cache.find(func);
    if (it != cache.end()) return it->second;
    FuncInfo info;
    info.mangled = func->getName();
    info.reachId = interner.get(func);
    const std::string& demangled = demangleFuncName(info.mangled);
    info.function = stripParams(demangled);
    // Never emit a nameless span: if a demangled form ever defeats the
    // parameter-list stripping, fall back to the full demangled (or raw) name.
    if (info.function.empty())
        info.function = demangled.empty() ? info.mangled : demangled;
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

/// Reachability bookkeeping for one entry. A call tree re-visits the same
/// name on many paths, so counting distinct names through a
/// `unordered_set<string>` hashes a long mangled symbol for every span. Names
/// are interned once per function; each mark is then an O(1) vector probe
/// against the current entry's epoch, with no string hashing or allocation
/// after the first visit.
struct ReachableTracker {
    unsigned epoch = 0;
    unsigned count = 0;
    std::vector<unsigned> seen;  ///< epoch stamp per interned mangled name

    void mark(const FuncInfo& info) {
        if (info.reachId >= seen.size())
            seen.resize(info.reachId + 1, 0);
        unsigned& stamp = seen[info.reachId];
        if (stamp != epoch) {
            stamp = epoch;
            ++count;
        }
    }
};

/// Cycle-cut bookkeeping for one entry. The call tree re-visits nodes once per
/// path, so a std::set here allocated a red-black-tree node on every push/pop.
/// SVF CallGraphNodes carry dense ids; an epoch-stamped array indexed by that
/// id makes probe/push/pop O(1) with no allocation after the first visit. A
/// fresh epoch per entry resets every stamp without clearing or rehashing.
struct RecursionGuard {
    unsigned epoch = 0;
    std::vector<unsigned> stamp;

private:
    unsigned& slot(const CallGraphNode* node) {
        auto id = node->getId();
        if (id >= stamp.size()) stamp.resize(id + 1, 0);
        return stamp[id];
    }

public:
    bool contains(const CallGraphNode* node) { return slot(node) == epoch; }
    void push(const CallGraphNode* node) { slot(node) = epoch; }
    void pop(const CallGraphNode* node) { slot(node) = 0; }
    void nextEpoch() { ++epoch; }  // start at 1 so a popped stamp (0) never matches
};

/// Recursively stream spans for the call graph rooted at \p node, mirroring the
/// tree traversal (one span per call edge, cycle-cut). Each span is handed to
/// \p sink as it is generated — the TSV sink writes it out immediately, the
/// tree sink buffers it — so no format ever materializes the whole report
/// unless it must; \p spanCount is the running span-id counter.
/// \p reachable tracks the unique functions reachable from the entry via an
/// epoch stamp (for the per-entry summary line). \p info is the descriptor the
/// caller already computed for \p node; passing it down avoids a second cache
/// lookup for every span.
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
                      const FuncInfo& info,
                      unsigned depth,
                      const std::optional<unsigned>& parentSpanId,
                      bool indirect,
                      const std::optional<CallSiteLoc>& callLoc,
                      bool suppressSpan,
                      RecursionGuard& in_stack,
                      const FunctionFilter& filter,
                      LLVMModuleSet* modSet,
                      std::unordered_map<const CallGraphEdge*, std::optional<CallSiteLoc>>& callSiteLocCache,
                      FuncInfoCache& infoCache,
                      NameInterner& interner,
                      SpanSink& sink,
                      unsigned& spanCount,
                      ReachableTracker& reachable,
                      bool excludeStd) {
    const FunObjVar* func = node->getFunction();
    if (!func || in_stack.contains(node) || func->isDeclaration()) return;

    // Hidden = library code, filter match, or a library callee reached from a
    // system-header call site. A user function is never hidden by the call
    // site alone, so "library calls user callback" survives rule 1.
    const bool hidden = info.isStd || info.filtered ||
                        (suppressSpan && !info.isApp);
    std::optional<unsigned> mySpanId;
    if (!hidden) {
        mySpanId = spanCount++;
        sink.span(info, *mySpanId, parentSpanId, indirect, depth, false, callLoc);
        reachable.mark(info);
    }

    if (node->OutEdgeBegin() == node->OutEdgeEnd()) return;

    in_stack.push(node);

    // Depth / parent for the next emitted node: bump only past emitted nodes.
    const std::optional<unsigned> childParent = hidden ? parentSpanId : mySpanId;
    const unsigned childDepth = hidden ? depth : depth + 1;

    for (auto eit = node->OutEdgeBegin(); eit != node->OutEdgeEnd(); ++eit) {
        CallGraphEdge* edge = *eit;
        CallGraphNode* callee = edge->getDstNode();
        if (!callee->getFunction()) continue;

        const FuncInfo& ci = getFuncInfo(callee->getFunction(), excludeStd,
                                         filter, infoCache, interner);
        // Rule 3 early exit: a hidden caller never follows an edge into a
        // callee already hidden by std/filter classification. Check this
        // before the call-site lookup — that lookup (and its first-time debug
        // info walk) is the expensive part, especially with many library
        // edges. The suppress-by-call-site case below still needs it.
        if (hidden && (ci.isStd || ci.filtered)) continue;

        const bool childIndirect = edge->isIndirectCallEdge();
        const std::optional<CallSiteLoc>& cl =
            getMinCallSiteLoc(edge, modSet, callSiteLocCache);

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

        if (in_stack.contains(callee)) {
            // Cycle back-edge: record the span but do not expand its subtree.
            if (calleeHidden) continue;
            sink.span(ci, spanCount++, childParent, childIndirect, childDepth,
                      true, cl);
            reachable.mark(ci);
            continue;
        }

        emitSpans(callee, ci, childDepth, childParent, childIndirect, cl,
                  calleeSuppress, in_stack, filter, modSet, callSiteLocCache,
                  infoCache, interner, sink, spanCount, reachable, excludeStd);
    }
    in_stack.pop(node);
}

/// Write the #-prefixed metadata header. Per-entry span lines are streamed
/// during the traversal, so the report is never materialized in memory.
static void writeHeader(std::ostream& os,
                        const llvm::Module& M,
                        const AnalysisConfig& config,
                        const std::vector<std::string>& entryNames) {
    os << "#schema_version 2\n";
    os << "#bitcode " << tsvEscape(M.getModuleIdentifier()) << "\n";
    os << "#analysis pta=" << analysisPtaName(config);
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
    NameInterner interner;
    FuncInfoCache infoCache;
    infoCache.reserve(M.size());  // upper bound on unique functions
    writeHeader(os, M, config, entryNames);
    ReachableTracker reachable;
    RecursionGuard in_stack;  // stamp vector reused across entries
    for (const std::string& name : entryNames) {
        CallGraphNode* entryNode = resolveEntry(callGraph, name);
        if (!entryNode) {
            llvm::errs() << "[WARN] entry function not found: " << name << "\n";
            continue;
        }
        const FuncInfo& entryInfo = getFuncInfo(entryNode->getFunction(),
                                                config.excludeStd, filter,
                                                infoCache, interner);
        if (entryInfo.filtered) {
            llvm::errs() << "[WARN] entry function matches a filter pattern, "
                            "skipped: " << name << "\n";
            continue;
        }
        sink.beginEntry(name);
        // A new epoch makes every cached FuncInfo unvisited again for this
        // entry without clearing or rehashing a set.
        ++reachable.epoch;
        reachable.count = 0;
        in_stack.nextEpoch();
        unsigned spanCount = 0;
        emitSpans(entryNode, entryInfo, 0, std::nullopt, /*indirect=*/false,
                  std::nullopt, false, in_stack, filter, modSet,
                  callSiteLocCache, infoCache, interner, sink, spanCount,
                  reachable, config.excludeStd);
        sink.endEntry(name, spanCount, reachable.count);
    }
    os.flush();
    if (os.good() && fb.close())
        llvm::outs() << "report written to " << outPath << "\n";
    else
        llvm::errs() << "[ERROR] failed to write output file: " << outPath << "\n";
}
