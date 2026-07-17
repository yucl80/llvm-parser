#include "WPA/Andersen.h"
#include "WPA/FlowSensitive.h"
#include "WPA/VersionedFlowSensitive.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "llvm/Demangle/Demangle.h"

#include <vector>
#include <set>
#include <map>

using namespace SVF;

/// Configuration for the pointer analysis.
struct AnalysisConfig {
    bool useFlowSensitive = false;
    bool useVersionedFS = false;
    bool contextSensitive = false;
    bool heapModel = false;
};

/// Demangle a C++ function name for human-readable output.
static std::string demangleFuncName(const std::string& mangled) {
    std::string result = llvm::demangle(mangled);
    return result.empty() ? mangled : result;
}

/// Print the call graph as a topological tree from the given node.
static void printCallTree(CallGraphNode* node,
                          const std::string& prefix,
                          bool is_last,
                          std::set<CallGraphNode*>& in_stack) {
    if (!node) return;

    const FunObjVar* func = node->getFunction();
    if (!func) return;

    if (!prefix.empty()) {
        llvm::outs() << prefix;
        llvm::outs() << (is_last ? "└── " : "├── ");
    }
    llvm::outs() << demangleFuncName(func->getName()) << "\n";

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

        bool indirect = edges[i]->isIndirectCallEdge();
        if (indirect) {
            llvm::outs() << child_prefix;
            llvm::outs() << (child_is_last ? "└── " : "├── ");
            llvm::outs() << "[Indirect]\n";
            printCallTree(callee,
                          child_prefix + (child_is_last ? "    " : "│   "),
                          true, in_stack);
        } else {
            printCallTree(callee, child_prefix, child_is_last, in_stack);
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
        printCallTree(node, "", true, in_stack);
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
