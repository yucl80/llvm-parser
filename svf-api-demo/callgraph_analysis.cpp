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
#include <vector>
#include <set>
#include <map>

using namespace SVF;

/// Demangle a C++ function name for human-readable output.
static std::string demangleFuncName(const std::string& mangled) {
    std::string result = llvm::demangle(mangled);
    return result.empty() ? mangled : result;
}

/// Source line range of a function definition, extracted from debug info.
struct FuncLineRange {
    std::string file;
    unsigned start = 0;
    unsigned end = 0;
    bool hasInfo = false;
};

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

/// Print the call graph as a topological tree from the given node.
static void printCallTree(CallGraphNode* node,
                          const std::string& prefix,
                          bool is_last,
                          std::set<CallGraphNode*>& in_stack,
                          const std::map<std::string, FuncLineRange>& lineRanges,
                          const std::vector<unsigned>& callLines) {
    if (!node) return;

    const FunObjVar* func = node->getFunction();
    if (!func) return;

    if (!prefix.empty()) {
        llvm::outs() << prefix;
        llvm::outs() << (is_last ? "└── " : "├── ");
    }
    llvm::outs() << demangleFuncName(func->getName());
    auto it = lineRanges.find(func->getName());
    if (it != lineRanges.end() && it->second.hasInfo) {
        llvm::outs() << "  [" << it->second.file << ":"
                     << it->second.start << "-" << it->second.end << "]";
    }
    if (!callLines.empty()) {
        llvm::outs() << "  [call@";
        for (size_t i = 0; i < callLines.size(); ++i) {
            if (i) llvm::outs() << ",";
            llvm::outs() << callLines[i];
        }
        llvm::outs() << "]";
    }
    llvm::outs() << "\n";

    std::vector<CallGraphEdge*> edges;
    for (auto it = node->OutEdgeBegin(); it != node->OutEdgeEnd(); ++it)
        edges.push_back(*it);

    if (edges.empty()) return;

    if (in_stack.count(node)) {
        llvm::outs() << prefix << (is_last ? "    " : "│   ")
                     << "└── [cycle]\n";
        return;
    }
    in_stack.insert(node);

    for (size_t i = 0; i < edges.size(); ++i) {
        std::string child_prefix = prefix + (is_last ? "    " : "│   ");
        bool child_is_last = (i == edges.size() - 1);

        CallGraphNode* callee = edges[i]->getDstNode();
        if (!callee->getFunction()) continue;

        std::vector<unsigned> callLines = getCallSiteLines(edges[i]);

        bool indirect = edges[i]->isIndirectCallEdge();
        if (indirect) {
            llvm::outs() << child_prefix;
            llvm::outs() << (child_is_last ? "└── " : "├── ");
            llvm::outs() << "[Indirect]\n";
            printCallTree(callee,
                          child_prefix + (child_is_last ? "    " : "│   "),
                          true, in_stack, lineRanges, callLines);
        } else {
            printCallTree(callee, child_prefix, child_is_last, in_stack, lineRanges, callLines);
        }
    }
    in_stack.erase(node);
}

void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config) {
    // Apply configuration options.
    // Notes:
    //   - Options::ContextInsensitive defaults to false, meaning the SVFG
    //     optimizer already builds a context-sensitive SVFG by default.
    //   - It is declared 'const', so it cannot be changed at runtime; but
    //     the default is already what we want (context-sensitive).
    //   - Other heap model options are non-const and can be set directly.
    if (config.contextSensitive) {
        llvm::outs() << "[Config] Context-sensitive SVFG enabled (default: ContextInsensitive=false)\n";
    }
    if (config.heapModel) {
        Options::ModelConsts.setValue(true);
        Options::ModelArrays.setValue(true);
        llvm::outs() << "[Config] Heap object model: ModelConsts=true, ModelArrays=true\n";
    }

    // 1. Build SVFModule
    LLVMModuleSet::buildSVFModule(M);

    // 2. Build SVFIR
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    // 3. Select and run pointer analysis
    PointerAnalysis* pta = nullptr;

    if (config.useFlowSensitive) {
        llvm::outs() << "[Analysis] FlowSensitive (FSSPARSE_WPA)\n";
        pta = FlowSensitive::createFSWPA(pag);
    } else if (config.useVersionedFS) {
        llvm::outs() << "[Analysis] VersionedFlowSensitive (VFS_WPA)\n";
        pta = VersionedFlowSensitive::createVFSWPA(pag);
    } else {
        llvm::outs() << "[Analysis] AndersenWaveDiff (baseline, flow-insensitive, context-insensitive)\n";
        pta = AndersenWaveDiff::createAndersenWaveDiff(pag);
    }

    pta->disablePrintStat();

    // 4. Get the CallGraph and print topology
    CallGraph* callGraph = pta->getCallGraph();

    // Map mangled function name -> definition line range (from debug info).
    std::map<std::string, FuncLineRange> lineRanges;
    for (llvm::Function& F : M) {
        FuncLineRange range;
        if (getFuncLineRange(&F, range))
            lineRanges[F.getName().str()] = range;
    }

    // Print header with mode info
    llvm::outs() << "===== Call Graph Topology ";
    if (config.useFlowSensitive)
        llvm::outs() << "[FlowSensitive";
    else if (config.useVersionedFS)
        llvm::outs() << "[VersionedFlowSensitive";
    else
        llvm::outs() << "[AndersenWaveDiff";
    if (config.contextSensitive)
        llvm::outs() << " + ContextSensitive";
    if (config.heapModel)
        llvm::outs() << " + HeapModel";
    llvm::outs() << "] =====\n";

    for (auto it = callGraph->begin(); it != callGraph->end(); ++it) {
        CallGraphNode* node = it->second;
        const FunObjVar* func = node->getFunction();
        if (!func) continue;

        std::string name = demangleFuncName(func->getName());
        if (name != "main") continue;

        std::set<CallGraphNode*> in_stack;
        std::vector<unsigned> rootCallLines;  // root has no incoming call site
        printCallTree(node, "", true, in_stack, lineRanges, rootCallLines);
    }

    // 5. Cleanup
    if (config.useFlowSensitive) {
        FlowSensitive::releaseFSWPA();
    } else if (config.useVersionedFS) {
        VersionedFlowSensitive::releaseVFSWPA();
    } else {
        AndersenWaveDiff::releaseAndersenWaveDiff();
    }
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
}
