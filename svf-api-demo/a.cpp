#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "llvm/Demangle/Demangle.h"

using namespace SVF;

/// Demangle a C++ function name for human-readable output.
/// Returns the demangled name (e.g. "Derived::virtualMethod()") on success,
/// or the original name if demangling fails.
static std::string demangleFuncName(const std::string& mangled) {
    std::string result = llvm::demangle(mangled);
    return result.empty() ? mangled : result;
}

void analyzeCallGraph(llvm::Module& M) {
    // 1. Build SVFModule (static void, modifies internal singleton)
    LLVMModuleSet::buildSVFModule(M);

    // 2. Build SVFIR (Program Assignment Graph)
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    // 3. Run Andersen-style pointer analysis (factory calls analyze() internally)
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

    // 4. Get the CallGraph
    CallGraph* callGraph = ander->getCallGraph();

    // 5. Traverse call graph nodes
    for (auto it = callGraph->begin(); it != callGraph->end(); ++it) {
        CallGraphNode* node = it->second;
        const FunObjVar* func = node->getFunction();
        if (!func) continue;

        std::string callerName = demangleFuncName(func->getName());

        // 6. Traverse outgoing call edges of this node
        for (auto edgeIt = node->OutEdgeBegin(); edgeIt != node->OutEdgeEnd(); ++edgeIt) {
            CallGraphEdge* edge = *edgeIt;
            CallGraphNode* dstNode = edge->getDstNode();
            const FunObjVar* dstFunc = dstNode->getFunction();
            if (!dstFunc) continue;

            std::string calleeName = demangleFuncName(dstFunc->getName());

            // Print call relation with full demangled names (includes class and signature)
            llvm::outs() << "Call: " << callerName << " -> " << calleeName;

            // Check for indirect call (function pointer / virtual function)
            if (edge->isIndirectCallEdge()) {
                llvm::outs() << " [Indirect Resolved]";
            }
            llvm::outs() << "\n";
        }
    }

    // 7. Cleanup
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
}
