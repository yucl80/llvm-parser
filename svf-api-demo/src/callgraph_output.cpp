#include "callgraph_output.h"

#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <charconv>
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

/// Source location of a call site: the file and line where the call text
/// appears in the analyzed source. An edge is often backed by several call
/// instructions (a loop, or the same helper reached from multiple sites); the
/// minimum line is the representative one, and its file is carried along.
struct CallSiteLoc {
    std::string file;
    unsigned line = 0;
};

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
    bool isStd = false;     ///< hidden from output when excludeStd is on
};

/// Lazy per-function descriptor cache, keyed by the function's FunObjVar
/// (unique per function; the name string is only materialized on a miss).
using FuncInfoCache = std::unordered_map<const FunObjVar*, FuncInfo>;

/// Compute (or fetch) the descriptor for a function. isStd is only filled when
/// excludeStd is on — otherwise nothing is excluded and the flag stays false.
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
    return cache.emplace(func, std::move(info)).first->second;
}

/// Forward declaration: writeSpanLine is defined below with the other writers.
static void writeSpanLine(std::ostream& os,
                          const FuncInfo& info,
                          unsigned span_id,
                          const std::optional<unsigned>& parent_span_id,
                          const std::string& kind,
                          unsigned depth,
                          bool cycle,
                          const std::optional<CallSiteLoc>& call_loc);

/// Recursively stream spans for the call graph rooted at \p node, mirroring the
/// tree traversal (one span per call edge, cycle-cut). Each span is written to
/// \p os immediately (one tab-separated line) so the report never has to be
/// materialized in memory; \p spanCount is the running span-id counter.
/// \p reachable collects the set of unique function names reachable from the
/// entry (for the per-entry summary line).
///
/// A span is emitted for \p node unless it is hidden: a standard-library /
/// system-header callee under \p excludeStd, or \p suppressSpan set (the edge
/// into this node has a system-header call site). Hidden nodes still traverse
/// their out-edges, so their user-code children are grafted onto the nearest
/// emitted ancestor (parent span / depth pass through unchanged) — a callback
/// invoked from inside library machinery still shows its own calls without the
/// library's span. Hidden nodes still join \p in_stack so recursion through
/// library machinery is cycle-cut.
static void emitSpans(CallGraphNode* node,
                      unsigned depth,
                      const std::optional<unsigned>& parentSpanId,
                      const std::string& kind,
                      const std::optional<CallSiteLoc>& callLoc,
                      bool suppressSpan,
                      std::set<CallGraphNode*>& in_stack,
                      const FunctionFilter& filter,
                      LLVMModuleSet* modSet,
                      std::unordered_map<const CallGraphEdge*, std::optional<CallSiteLoc>>& callSiteLocCache,
                      FuncInfoCache& infoCache,
                      std::ostream& os,
                      unsigned& spanCount,
                      std::unordered_set<std::string>& reachable,
                      bool excludeStd) {
    const FunObjVar* func = node->getFunction();
    if (!func || in_stack.count(node) || func->isDeclaration()) return;

    const FuncInfo& info = getFuncInfo(func, excludeStd, filter, infoCache);

    // Hidden nodes (standard-library/system-header callees under excludeStd,
    // or any callee reached through a system-header call site) produce no span
    // of their own; their user-code children are grafted onto the nearest
    // emitted ancestor (parentSpanId unchanged).
    const bool hidden = info.isStd || suppressSpan;
    std::optional<unsigned> mySpanId;
    if (!hidden) {
        const unsigned sid = spanCount++;
        writeSpanLine(os, info, sid, parentSpanId, kind, depth, false, callLoc);
        reachable.insert(info.mangled);
        mySpanId = sid;
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

        bool indirect = edge->isIndirectCallEdge();
        std::optional<CallSiteLoc> cl = getMinCallSiteLoc(edge, modSet, callSiteLocCache);
        std::string childKind = indirect ? "indirect" : "direct";

        // A call that happens inside a system include header is a standard-
        // library call (C/C++/LLVM headers under /usr/): its span is not part
        // of the analyzed program's own call graph. The callee's own calls,
        // made from user code, still appear grafted onto the emitted ancestor.
        const bool suppressChild = cl && isSystemHeaderFile(cl->file);

        if (in_stack.count(callee)) {
            // Cycle back-edge: record the span but do not expand its subtree.
            const FuncInfo& ci = getFuncInfo(callee->getFunction(), excludeStd,
                                             filter, infoCache);
            if (ci.isStd || suppressChild) continue;  // library call: no span
            writeSpanLine(os, ci, spanCount++, childParent, childKind, childDepth, true, cl);
            reachable.insert(ci.mangled);
            continue;
        }

        emitSpans(callee, childDepth, childParent, childKind, cl, suppressChild,
                  in_stack, filter, modSet, callSiteLocCache, infoCache, os,
                  spanCount, reachable, excludeStd);
    }
    in_stack.erase(node);
}

/// Escape a string for embedding in a tab-separated field: the separators that
/// would break the line (tab, newline, CR) plus the escape character itself.
static std::string tsvEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += static_cast<char>(c);
        }
    }
    return out;
}

/// Append a non-negative integer without iostream formatting overhead
/// (locale, num_put): std::to_chars is allocation- and locale-free.
static inline void appendNum(std::string& out, unsigned v) {
    char tmp[16];
    std::to_chars_result r = std::to_chars(tmp, tmp + sizeof(tmp), v);
    out.append(tmp, static_cast<size_t>(r.ptr - tmp));
}

/// Append a field, escaping the tab/newline/CR/backslash separators. The
/// common case (no specials) is a single find + append — no allocation; only
/// fields that actually contain a special character pay the escaping loop.
static inline void appendEscaped(std::string& out, const std::string& in) {
    if (in.find_first_of("\\\t\n\r") == std::string::npos) {
        out.append(in);
        return;
    }
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c);
        }
    }
}

/// Write one span as a tab-separated line:
///   span_id  parent_span_id  kind  depth  cycle  function  mangled  file
///   start_line  end_line  call_file  call_line
/// Empty fields mean "absent" (no parent, no debug line info, no call-site
/// location).
///
/// The line is assembled in a reusable buffer and written in one shot: the
/// per-span cost drops from ~11 ostream << calls (each a streambuf virtual
/// call) to a single os.write, and fields that need no escaping skip the
/// per-field string allocation. The thread-local buffer's capacity is reused
/// across spans, so the allocation cost amortizes to once. The per-function
/// strings come from the descriptor cache, so no span is ever copied.
static void writeSpanLine(std::ostream& os,
                          const FuncInfo& info,
                          unsigned span_id,
                          const std::optional<unsigned>& parent_span_id,
                          const std::string& kind,
                          unsigned depth,
                          bool cycle,
                          const std::optional<CallSiteLoc>& call_loc) {
    thread_local std::string buf;
    buf.clear();
    appendNum(buf, span_id);
    buf.push_back('\t');
    if (parent_span_id) appendNum(buf, *parent_span_id);
    buf.push_back('\t');
    buf.append(kind);
    buf.push_back('\t');
    appendNum(buf, depth);
    buf.push_back('\t');
    buf.push_back(cycle ? '1' : '0');
    buf.push_back('\t');
    appendEscaped(buf, info.function);
    buf.push_back('\t');
    appendEscaped(buf, info.mangled);
    buf.push_back('\t');
    appendEscaped(buf, info.file);
    buf.push_back('\t');
    if (info.hasLines) appendNum(buf, info.start);
    buf.push_back('\t');
    if (info.hasLines) appendNum(buf, info.end);
    buf.push_back('\t');
    if (call_loc) appendEscaped(buf, call_loc->file);
    buf.push_back('\t');
    if (call_loc) appendNum(buf, call_loc->line);
    buf.push_back('\n');
    os.write(buf.data(), static_cast<std::streamsize>(buf.size()));
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

/// Default report path: "<bitcode-stem>.callgraph.tsv".
static std::string defaultOutputPath(const llvm::Module& M) {
    std::string id = M.getModuleIdentifier();
    size_t dot = id.rfind('.');
    if (!id.empty() && dot != std::string::npos)
        id = id.substr(0, dot);
    return (id.empty() ? "callgraph" : id) + ".callgraph.tsv";
}

void writeCallGraphReport(const llvm::Module& M,
                          const AnalysisConfig& config,
                          CallGraph* callGraph,
                          const FunctionFilter& filter) {
    // Determine entries (default: main).
    std::vector<std::string> entryNames = config.entries;
    if (entryNames.empty())
        entryNames.push_back("main");

    // Stream the report to a file: header, then one #entry/#end block per
    // resolved entry with its spans written as they are generated. Peak
    // memory is O(depth + unique functions + unique edges), not O(span
    // count), so multi-million-span trees no longer balloon. The reverse
    // value map and the per-edge call-site-location cache are shared across
    // entries (edges are stable objects), so call locations are extracted
    // once.
    std::string outPath = config.outputFile.empty()
        ? defaultOutputPath(M)
        : config.outputFile;
    static char outBuf[4 << 20];
    std::filebuf fb;
    fb.pubsetbuf(outBuf, sizeof(outBuf));
    if (!fb.open(outPath.c_str(), std::ios::out)) {
        llvm::errs() << "[ERROR] cannot open output file: " << outPath << "\n";
        return;
    }

    std::ostream os(&fb);
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
        os << "#entry " << tsvEscape(name) << "\n";
        unsigned spanCount = 0;
        std::set<CallGraphNode*> in_stack;
        std::unordered_set<std::string> reachable;
        reachable.reserve(M.size());
        emitSpans(entryNode, 0, std::nullopt, "root", std::nullopt, false,
                  in_stack, filter, modSet, callSiteLocCache, infoCache,
                  os, spanCount, reachable, config.excludeStd);
        os << "#end " << tsvEscape(name) << " spans=" << spanCount
           << " reachable_funcs=" << reachable.size() << "\n";
    }
    os.flush();
    if (os.good() && fb.close())
        llvm::outs() << "report written to " << outPath << "\n";
    else
        llvm::errs() << "[ERROR] failed to write output file: " << outPath << "\n";
}
