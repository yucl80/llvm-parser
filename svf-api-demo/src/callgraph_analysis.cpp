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
#include <cstdio>
#include <fstream>
#include <optional>
#include <ostream>
#include <vector>
#include <set>
#include <map>

using namespace SVF;

/// Demangle a C++ function name for human-readable output.
static std::string demangleFuncName(const std::string& mangled) {
    std::string result = llvm::demangle(mangled);
    return result.empty() ? mangled : result;
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

/// Collect the source line numbers of the call site(s) backing an edge.
/// Each call site is an LLVM call instruction, recovered from the edge's
/// CallICFGNode via LLVMModuleSet's reverse value map. The outermost debug
/// location is used so inlined call text still reports the caller's line.
static std::vector<unsigned> getCallSiteLines(const CallGraphEdge* edge) {
    std::set<unsigned> lines;
    LLVMModuleSet* modSet = LLVMModuleSet::getLLVMModuleSet();
    auto collect = [&](CallGraphEdge::CallInstSet::const_iterator begin,
                       CallGraphEdge::CallInstSet::const_iterator end) {
        for (auto it = begin; it != end; ++it) {
            if (!modSet->hasLLVMValue(*it)) continue;
            const auto* call = llvm::dyn_cast_or_null<const llvm::CallBase>(
                modSet->getLLVMValue(*it));
            const llvm::DILocation* loc = call ? call->getDebugLoc() : nullptr;
            if (!loc) continue;
            while (loc->getInlinedAt()) loc = loc->getInlinedAt();
            lines.insert(loc->getLine());
        }
    };
    collect(edge->directCallsBegin(), edge->directCallsEnd());
    collect(edge->indirectCallsBegin(), edge->indirectCallsEnd());
    return std::vector<unsigned>(lines.begin(), lines.end());
}

/// One invocation record in a trace, expressed like a distributed-tracing
/// span: parent_span_id links it to the calling span; the spans array order
/// is the pre-order (call order) traversal.
struct Span {
    unsigned span_id = 0;
    std::optional<unsigned> parent_span_id;
    std::string function;   ///< demangled name
    std::string mangled;    ///< raw symbol name
    std::string file;
    std::optional<unsigned> start_line;
    std::optional<unsigned> end_line;
    std::optional<unsigned> call_line;  ///< line in the caller that made the call
    std::string kind;       ///< root | direct | indirect
    unsigned depth = 0;
    bool cycle = false;     ///< recursion back-edge: subtree not expanded
};

/// Recursively emit spans for the call graph rooted at \p node, mirroring the
/// tree traversal (one span per call edge, cycle-cut). \p reachable collects
/// the set of unique function names reachable from the entry.
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
                      std::vector<Span>& spans,
                      std::set<std::string>& reachable,
                      bool excludeStd) {
    const FunObjVar* func = node->getFunction();
    if (!func || in_stack.count(node)) return;

    const std::string mangled = func->getName();
    const std::string demangled = demangleFuncName(mangled);
    const bool isStd = excludeStd && isExcludedFunction(mangled, demangled, lineRanges);

    // Std-library / system-header nodes are skipped in the output; their
    // user-code children are grafted onto the nearest non-std ancestor
    // (parentSpanId unchanged).
    std::optional<unsigned> mySpanId;
    if (!isStd) {
        Span s;
        s.span_id = spans.size();
        s.parent_span_id = parentSpanId;
        s.function = stripParams(demangled);
        s.mangled = mangled;
        auto lit = lineRanges.find(mangled);
        if (lit != lineRanges.end() && lit->second.hasInfo) {
            s.file = lit->second.file;
            s.start_line = lit->second.start;
            s.end_line = lit->second.end;
        }
        s.call_line = callLine;
        s.kind = kind;
        s.depth = depth;
        spans.push_back(s);
        reachable.insert(mangled);
        mySpanId = s.span_id;
    }

    std::vector<CallGraphEdge*> edges;
    for (auto it = node->OutEdgeBegin(); it != node->OutEdgeEnd(); ++it)
        edges.push_back(*it);
    if (edges.empty()) return;

    in_stack.insert(node);

    // Depth / parent for the next emitted node: bump only past emitted nodes.
    const std::optional<unsigned> childParent = isStd ? parentSpanId : mySpanId;
    const unsigned childDepth = isStd ? depth : depth + 1;

    for (CallGraphEdge* edge : edges) {
        CallGraphNode* callee = edge->getDstNode();
        if (!callee->getFunction()) continue;

        bool indirect = edge->isIndirectCallEdge();
        std::vector<unsigned> lines = getCallSiteLines(edge);
        std::optional<unsigned> cl = lines.empty()
            ? std::optional<unsigned>{}
            : std::optional<unsigned>(lines.front());
        std::string childKind = indirect ? "indirect" : "direct";

        if (in_stack.count(callee)) {
            // Cycle back-edge: record the span but do not expand its subtree.
            const std::string cm = callee->getFunction()->getName();
            if (excludeStd && isExcludedFunction(cm, demangleFuncName(cm), lineRanges))
                continue;  // std / system-header nodes are not part of the output
            Span cs;
            cs.span_id = spans.size();
            cs.parent_span_id = childParent;
            cs.function = stripParams(demangleFuncName(cm));
            cs.mangled = cm;
            auto cit = lineRanges.find(cm);
            if (cit != lineRanges.end() && cit->second.hasInfo) {
                cs.file = cit->second.file;
                cs.start_line = cit->second.start;
                cs.end_line = cit->second.end;
            }
            cs.call_line = cl;
            cs.kind = childKind;
            cs.depth = childDepth;
            cs.cycle = true;
            spans.push_back(cs);
            reachable.insert(cm);
            continue;
        }

        emitSpans(callee, childDepth, childParent, childKind, cl,
                  in_stack, lineRanges, spans, reachable, excludeStd);
    }
    in_stack.erase(node);
}

/// Escape a string for embedding in a JSON string literal.
static std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    char buf[8];
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/// Write one span object.
static void writeSpanJson(std::ostream& os, const Span& s) {
    auto numOrNull = [&os](const std::optional<unsigned>& v) {
        if (v) os << *v;
        else   os << "null";
    };
    auto strOrNull = [&os](const std::string& v) {
        if (v.empty()) os << "null";
        else           os << "\"" << jsonEscape(v) << "\"";
    };

    os << "        { "
       << "\"span_id\": " << s.span_id << ", "
       << "\"parent_span_id\": ";
    numOrNull(s.parent_span_id);
    os << ", "
       << "\"function\": \"" << jsonEscape(s.function) << "\", "
       << "\"mangled\": \"" << jsonEscape(s.mangled) << "\", "
       << "\"file\": ";
    strOrNull(s.file);
    os << ", "
       << "\"start_line\": ";
    numOrNull(s.start_line);
    os << ", "
       << "\"end_line\": ";
    numOrNull(s.end_line);
    os << ", "
       << "\"call_line\": ";
    numOrNull(s.call_line);
    os << ", "
       << "\"kind\": \"" << s.kind << "\", "
       << "\"depth\": " << s.depth << ", "
       << "\"cycle\": " << (s.cycle ? "true" : "false")
       << " }";
}

/// Serialize the whole report: top-level metadata + one callgraph per entry.
static void writeReportJson(std::ostream& os,
                            const llvm::Module& M,
                            const AnalysisConfig& config,
                            const std::vector<std::string>& entryNames,
                            const std::map<std::string, std::vector<Span>>& entrySpans,
                            const std::map<std::string, size_t>& reachableCounts) {
    os << "{\n";

    // Metadata.
    os << "  \"schema_version\": 1,\n";
    os << "  \"bitcode\": \"" << jsonEscape(M.getModuleIdentifier()) << "\",\n";
    os << "  \"analysis\": { \"pta\": \"";
    if (config.useFlowSensitive)
        os << "FlowSensitive";
    else if (config.useVersionedFS)
        os << "VersionedFlowSensitive";
    else
        os << "AndersenWaveDiff";
    os << "\", \"context_sensitive\": " << (config.contextSensitive ? "true" : "false")
       << ", \"heap_model\": " << (config.heapModel ? "true" : "false")
       << ", \"exclude_std\": " << (config.excludeStd ? "true" : "false") << " },\n";

    // Entries.
    os << "  \"entries\": [";
    for (size_t i = 0; i < entryNames.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << jsonEscape(entryNames[i]) << "\"";
    }
    os << "],\n";

    // Per-entry call graphs.
    os << "  \"callgraphs\": {\n";
    bool first = true;
    for (const std::string& name : entryNames) {
        auto sit = entrySpans.find(name);
        if (sit == entrySpans.end()) continue;
        const std::vector<Span>& spans = sit->second;
        if (!first) os << ",\n";
        first = false;
        os << "    \"" << jsonEscape(name) << "\": {\n";
        os << "      \"span_count\": " << spans.size() << ",\n";
        auto rit = reachableCounts.find(name);
        os << "      \"reachable_funcs\": " << (rit != reachableCounts.end() ? rit->second : 0)
           << ",\n";
        os << "      \"spans\": [\n";
        for (size_t i = 0; i < spans.size(); ++i) {
            if (i) os << ",\n";
            writeSpanJson(os, spans[i]);
        }
        os << "\n      ]\n";
        os << "    }";
    }
    os << "\n  }\n";
    os << "}\n";
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

/// Default report path: "<bitcode-stem>.callgraph.json".
static std::string defaultOutputPath(const llvm::Module& M) {
    std::string id = M.getModuleIdentifier();
    size_t dot = id.rfind('.');
    if (!id.empty() && dot != std::string::npos)
        id = id.substr(0, dot);
    return (id.empty() ? "callgraph" : id) + ".callgraph.json";
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
        pta = new AndersenWaveDiff(pag, Andersen::AndersenWaveDiff_WPA, false);
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

    // Emit one call graph (spans) per entry.
    std::map<std::string, std::vector<Span>> entrySpans;
    std::map<std::string, size_t> reachableCounts;
    for (const std::string& name : entryNames) {
        CallGraphNode* entryNode = resolveEntry(callGraph, name);
        if (!entryNode) {
            llvm::errs() << "[WARN] entry function not found: " << name << "\n";
            continue;
        }
        std::set<CallGraphNode*> in_stack;
        std::set<std::string> reachable;
        std::vector<Span> spans;
        emitSpans(entryNode, 0, std::nullopt, "root", std::nullopt,
                  in_stack, lineRanges, spans, reachable, config.excludeStd);
        entrySpans[name] = std::move(spans);
        reachableCounts[name] = reachable.size();
    }

    // 5. Write the JSON report to a file.
    std::string outPath = config.outputFile.empty()
        ? defaultOutputPath(M)
        : config.outputFile;
    std::ofstream ofs(outPath);
    if (!ofs) {
        llvm::errs() << "[ERROR] cannot open output file: " << outPath << "\n";
    } else {
        writeReportJson(ofs, M, config, entryNames, entrySpans, reachableCounts);
        ofs.close();
        if (ofs.good())
            llvm::outs() << "report written to " << outPath << "\n";
        else
            llvm::errs() << "[ERROR] failed to write output file: " << outPath << "\n";
    }

    // 6. Cleanup
    delete pta;
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
}
