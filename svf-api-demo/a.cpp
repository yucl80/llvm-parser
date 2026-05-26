#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "llvm/Demangle/Demangle.h"

#include <vector>
#include <set>
#include <map>

using namespace SVF;

/// Demangle a C++ function name for human-readable output.
/// Returns the demangled name (e.g. "Derived::virtualMethod()") on success,
/// or the original name if demangling fails.
static std::string demangleFuncName(const std::string& mangled) {
    std::string result = llvm::demangle(mangled);
    return result.empty() ? mangled : result;
}

/// Print the call graph as a topological tree from the given node.
/// `in_stack` tracks nodes currently on the recursion stack to detect cycles.
static void printCallTree(CallGraphNode* node,
                          const std::string& prefix,
                          bool is_last,
                          std::set<CallGraphNode*>& in_stack) {
    if (!node) return;

    const FunObjVar* func = node->getFunction();
    if (!func) return;

    // Print connector and node name
    if (!prefix.empty()) {
        llvm::outs() << prefix;
        llvm::outs() << (is_last ? "└── " : "├── ");
    }
    llvm::outs() << demangleFuncName(func->getName()) << "\n";

    // Collect outgoing edges
    std::vector<CallGraphEdge*> edges;
    for (auto it = node->OutEdgeBegin(); it != node->OutEdgeEnd(); ++it)
        edges.push_back(*it);

    if (edges.empty()) return;

    // Detect cycle: if this node is already on the current path, mark and stop
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
                          true,
                          in_stack);
        } else {
            printCallTree(callee, child_prefix, child_is_last, in_stack);
        }
    }

    in_stack.erase(node);
}

void analyzeCallGraph(llvm::Module& M) {
    // 1. Build SVFModule (static void, modifies internal singleton)
    LLVMModuleSet::buildSVFModule(M);

    // 2. Build SVFIR (Program Assignment Graph)
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    // 3. Run Andersen-style pointer analysis (factory calls analyze() internally)
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    ander->disablePrintStat();

    // 4. Get the CallGraph
    CallGraph* callGraph = ander->getCallGraph();

    // 5. Print the call graph topology starting from main
    llvm::outs() << "===== Call Graph Topology =====\n";
    for (auto it = callGraph->begin(); it != callGraph->end(); ++it) {
        CallGraphNode* node = it->second;
        const FunObjVar* func = node->getFunction();
        if (!func) continue;

        std::string name = demangleFuncName(func->getName());
        if (name != "main") continue;

        std::set<CallGraphNode*> in_stack;
        printCallTree(node, "", true, in_stack);
    }

    // 6. Cleanup
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
}
