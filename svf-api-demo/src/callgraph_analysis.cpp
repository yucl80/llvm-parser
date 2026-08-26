#include "callgraph_analysis.h"

#include "WPA/Andersen.h"
#include "WPA/FlowSensitive.h"
#include "WPA/VersionedFlowSensitive.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <ostream>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>

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

/// Strip a trailing parameter list: "foo(int)" -> "foo".
static std::string stripParams(std::string s) {
    size_t p = s.find('(');
    if (p != std::string::npos)
        s.erase(p);
    return s;
}

/// True for C++ standard library / runtime functions hidden from the output
/// call graph by default: the std:: and __gnu_cxx:: namespaces, the __cxa_*
/// exception ABI, the allocation operators, and clang's terminate helper.
/// User callbacks called from inside these (comparators, lambdas, functors)
/// are still shown, grafted onto the caller that reached them.
static bool isStdLibFunction(const std::string& demangled) {
    if (demangled.rfind("std::", 0) == 0) return true;
    if (demangled.rfind("__gnu_cxx::", 0) == 0) return true;
    if (demangled.rfind("__cxa_", 0) == 0) return true;
    if (demangled == "__clang_call_terminate") return true;
    // operator new/delete carry parameter lists ("operator new(unsigned long)");
    // strip them before the exact-name match.
    const std::string base = stripParams(demangled);
    return base == "operator new" || base == "operator new[]" ||
           base == "operator delete" || base == "operator delete[]";
}

/// True when a function's definition file lives in a system include directory
/// (Linux: anything absolute under /usr/ — libstdc++, glibc headers, clang
/// builtins). Relative paths are the analyzed program's own sources and are
/// kept. Functions with no debug info (empty file) are not file-filtered.
static bool isSystemHeaderFile(const std::string& file) {
    if (file.empty() || file[0] != '/')
        return false;
    return file.rfind("/usr/", 0) == 0;
}

/// Source line range of a function definition, extracted from debug info.
struct FuncLineRange {
    std::string file;
    unsigned start = 0;
    unsigned end = 0;
    bool hasInfo = false;
};

/// Whether a function is hidden from the output call graph: either its
/// demangled name is a C++ standard-library / runtime symbol (std::,
/// __gnu_cxx::, __cxa_*, operator new/delete), or its definition lives in a
/// system include header. The name check alone is insufficient — libstdc++
/// templates often demangle with a return-type prefix ("void std::...",
/// "decltype(...) std::...") or even to a user-looking name, so the file
/// location is the reliable signal.
static bool isExcludedFunction(const std::string& mangled,
                               const std::string& demangled,
                               const std::map<std::string, FuncLineRange>& lineRanges) {
    if (isStdLibFunction(demangled))
        return true;
    auto it = lineRanges.find(mangled);
    if (it != lineRanges.end())
        return isSystemHeaderFile(it->second.file);
    // No debug info for this function: it is library/runtime code (libc,
    // LLVM intrinsics, RTTI), not part of the analyzed program's own sources.
    // Only exclude when the module does carry debug info elsewhere — otherwise
    // the program was built without -g and nothing is attributable.
    return !lineRanges.empty();
}

/// Compute a function's definition line range from its DISubprogram.
/// The start line is the function signature line; the end line is the
/// largest debug line among the function's own (non-inlined) instructions.
/// Returns false when the bitcode has no debug info (not built with -g).
static bool getFuncLineRange(const llvm::Function* F, FuncLineRange& range) {
    const llvm::DISubprogram* sp = F->getSubprogram();
    if (!sp) return false;

    range.file = sp->getFilename().str();
    range.start = sp->getLine();
    range.end = range.start;

    for (const llvm::BasicBlock& bb : *F) {
        for (const llvm::Instruction& inst : bb) {
            const llvm::DILocation* loc = inst.getDebugLoc();
            if (!loc || loc->getInlinedAt()) continue; // skip inlined callee code
            range.end = std::max(range.end, loc->getLine());
        }
    }
    range.hasInfo = true;
    return true;
}

/// Minimum source line of the call site(s) backing an edge, cached per edge.
/// Each call site is an LLVM call instruction, recovered from the edge's
/// CallICFGNode via LLVMModuleSet's reverse value map. The outermost debug
/// location is used so inlined call text still reports the caller's line.
///
/// Only the minimum line is needed (the caller uses front() of the previously
/// sorted set), and an edge is re-traversed for every path that reaches it, so
/// the result is computed once per unique edge and memoized.
static std::optional<unsigned> getMinCallSiteLine(
    const CallGraphEdge* edge,
    LLVMModuleSet* modSet,
    std::unordered_map<const CallGraphEdge*, std::optional<unsigned>>& cache) {
    auto it = cache.find(edge);
    if (it != cache.end()) return it->second;

    std::optional<unsigned> minLine;
    auto collect = [&](CallGraphEdge::CallInstSet::const_iterator begin,
                       CallGraphEdge::CallInstSet::const_iterator end) {
        for (auto vit = begin; vit != end; ++vit) {
            if (!modSet->hasLLVMValue(*vit)) continue;
            const auto* call = llvm::dyn_cast_or_null<const llvm::CallBase>(
                modSet->getLLVMValue(*vit));
            const llvm::DILocation* loc = call ? call->getDebugLoc() : nullptr;
            if (!loc) continue;
            while (loc->getInlinedAt()) loc = loc->getInlinedAt();
            if (!minLine || loc->getLine() < *minLine)
                minLine = loc->getLine();
        }
    };
    collect(edge->directCallsBegin(), edge->directCallsEnd());
    collect(edge->indirectCallsBegin(), edge->indirectCallsEnd());
    cache.emplace(edge, minLine);
    return minLine;
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
                                   const std::map<std::string, FuncLineRange>& lineRanges,
                                   FuncInfoCache& cache) {
    auto it = cache.find(func);
    if (it != cache.end()) return it->second;
    FuncInfo info;
    info.mangled = func->getName();
    const std::string& demangled = demangleFuncName(info.mangled);
    info.function = stripParams(demangled);
    auto lit = lineRanges.find(info.mangled);
    if (lit != lineRanges.end() && lit->second.hasInfo) {
        info.file = lit->second.file;
        info.start = lit->second.start;
        info.end = lit->second.end;
        info.hasLines = true;
    }
    if (excludeStd)
        info.isStd = isExcludedFunction(info.mangled, demangled, lineRanges);
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
                          const std::optional<unsigned>& call_line);

/// Recursively stream spans for the call graph rooted at \p node, mirroring the
/// tree traversal (one span per call edge, cycle-cut). Each span is written to
/// \p os immediately (one tab-separated line) so the report never has to be
/// materialized in memory; \p spanCount is the running span-id counter.
/// \p reachable collects the set of unique function names reachable from the
/// entry (for the per-entry summary line).
///
/// When \p excludeStd is set, standard-library nodes produce no span: their
/// user-code children are grafted onto the nearest non-std ancestor (the
/// parent span / depth pass through unchanged), so callbacks invoked from
/// inside std functions still appear. Std nodes still join \p in_stack so
/// recursion through std machinery is cycle-cut.
static void emitSpans(CallGraphNode* node,
                      unsigned depth,
                      const std::optional<unsigned>& parentSpanId,
                      const std::string& kind,
                      const std::optional<unsigned>& callLine,
                      std::set<CallGraphNode*>& in_stack,
                      const std::map<std::string, FuncLineRange>& lineRanges,
                      LLVMModuleSet* modSet,
                      std::unordered_map<const CallGraphEdge*, std::optional<unsigned>>& callLineCache,
                      FuncInfoCache& infoCache,
                      std::ostream& os,
                      unsigned& spanCount,
                      std::unordered_set<std::string>& reachable,
                      bool excludeStd) {
    const FunObjVar* func = node->getFunction();
    if (!func || in_stack.count(node)) return;

    const FuncInfo& info = getFuncInfo(func, excludeStd, lineRanges, infoCache);

    // Std-library / system-header nodes are skipped in the output; their
    // user-code children are grafted onto the nearest non-std ancestor
    // (parentSpanId unchanged).
    std::optional<unsigned> mySpanId;
    if (!info.isStd) {
        const unsigned sid = spanCount++;
        writeSpanLine(os, info, sid, parentSpanId, kind, depth, false, callLine);
        reachable.insert(info.mangled);
        mySpanId = sid;
    }

    if (node->OutEdgeBegin() == node->OutEdgeEnd()) return;

    in_stack.insert(node);

    // Depth / parent for the next emitted node: bump only past emitted nodes.
    const std::optional<unsigned> childParent = info.isStd ? parentSpanId : mySpanId;
    const unsigned childDepth = info.isStd ? depth : depth + 1;

    for (auto eit = node->OutEdgeBegin(); eit != node->OutEdgeEnd(); ++eit) {
        CallGraphEdge* edge = *eit;
        CallGraphNode* callee = edge->getDstNode();
        if (!callee->getFunction()) continue;

        bool indirect = edge->isIndirectCallEdge();
        std::optional<unsigned> cl = getMinCallSiteLine(edge, modSet, callLineCache);
        std::string childKind = indirect ? "indirect" : "direct";

        if (in_stack.count(callee)) {
            // Cycle back-edge: record the span but do not expand its subtree.
            const FuncInfo& ci = getFuncInfo(callee->getFunction(), excludeStd, lineRanges, infoCache);
            if (ci.isStd) continue;  // std / system-header nodes are not part of the output
            writeSpanLine(os, ci, spanCount++, childParent, childKind, childDepth, true, cl);
            reachable.insert(ci.mangled);
            continue;
        }

        emitSpans(callee, childDepth, childParent, childKind, cl,
                  in_stack, lineRanges, modSet, callLineCache, infoCache,
                  os, spanCount, reachable, excludeStd);
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
///   start_line  end_line  call_line
/// Empty fields mean "absent" (no parent, no debug line info).
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
                          const std::optional<unsigned>& call_line) {
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
    if (call_line) appendNum(buf, *call_line);
    buf.push_back('\n');
    os.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

/// Write the #-prefixed metadata header. Per-entry span lines are streamed by
/// analyzeCallGraph during the traversal, so the report is never materialized
/// in memory.
static void writeHeader(std::ostream& os,
                        const llvm::Module& M,
                        const AnalysisConfig& config,
                        const std::vector<std::string>& entryNames) {
    os << "#schema_version 1\n";
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

void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config) {
    const char* ptaName = config.useFlowSensitive ? "FlowSensitive"
                        : config.useVersionedFS ? "VersionedFlowSensitive"
                        : "AndersenWaveDiff";
    llvm::errs() << "[Config] pta=" << ptaName
                 << " context_sensitive=" << (config.contextSensitive ? "true" : "false")
                 << " heap_model=" << (config.heapModel ? "true" : "false")
                 << " exclude_std=" << (config.excludeStd ? "true" : "false") << "\n";

    if (config.heapModel) {
        Options::ModelConsts.setValue(true);
        Options::ModelArrays.setValue(true);
        llvm::errs() << "[Config] Heap object model: ModelConsts=true, ModelArrays=true\n";
    }

    // 1. Build SVFModule
    LLVMModuleSet::buildSVFModule(M);

    // 2. Build SVFIR
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    // 3. Select and run pointer analysis. The static create* helpers run
    //    analyze() during construction, before disablePrintStat() could take
    //    effect, so we construct manually to keep SVF's statistics off stdout.
    PointerAnalysis* pta = nullptr;
    if (config.useFlowSensitive) {
        pta = new FlowSensitive(pag);
    } else if (config.useVersionedFS) {
        pta = new VersionedFlowSensitive(pag);
    } else {
        pta = new AndersenWaveDiff(pag, PTATY::AndersenWaveDiff_WPA, false);
    }
    pta->disablePrintStat();
    pta->analyze();

    // 4. Get the CallGraph
    CallGraph* callGraph = pta->getCallGraph();

    // Map mangled function name -> definition line range (from debug info).
    std::map<std::string, FuncLineRange> lineRanges;
    for (llvm::Function& F : M) {
        FuncLineRange range;
        if (getFuncLineRange(&F, range))
            lineRanges[F.getName().str()] = range;
    }

    // Determine entries (default: main).
    std::vector<std::string> entryNames = config.entries;
    if (entryNames.empty())
        entryNames.push_back("main");

    // 5. Stream the report to a file: header, then one #entry/#end block per
    //    resolved entry with its spans written as they are generated. Peak
    //    memory is O(depth + unique functions + unique edges), not O(span
    //    count), so multi-million-span trees no longer balloon. The reverse
    //    value map and the per-edge call-line cache are shared across entries
    //    (edges are stable objects), so call lines are extracted once.
    std::string outPath = config.outputFile.empty()
        ? defaultOutputPath(M)
        : config.outputFile;
    static char outBuf[4 << 20];
    std::filebuf fb;
    fb.pubsetbuf(outBuf, sizeof(outBuf));
    if (!fb.open(outPath.c_str(), std::ios::out)) {
        llvm::errs() << "[ERROR] cannot open output file: " << outPath << "\n";
    } else {
        std::ostream os(&fb);
        LLVMModuleSet* modSet = LLVMModuleSet::getLLVMModuleSet();
        std::unordered_map<const CallGraphEdge*, std::optional<unsigned>> callLineCache;
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
            emitSpans(entryNode, 0, std::nullopt, "root", std::nullopt,
                      in_stack, lineRanges, modSet, callLineCache, infoCache,
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

    // 6. Cleanup
    delete pta;
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
}
