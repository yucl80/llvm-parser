#include "callgraph_analysis.h"

#include "callgraph_output.h"
#include "function_filter.h"

#include "WPA/Andersen.h"
#include "WPA/FlowSensitive.h"
#include "WPA/VersionedFlowSensitive.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

#include <memory>

using namespace SVF;

int analyzeCallGraph(const std::string& bcFile, const AnalysisConfig& config) {
    llvm::SMDiagnostic err;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(bcFile, err, ctx);
    if (!mod) {
        llvm::errs() << "Failed to load bitcode file: " << bcFile << "\n";
        return 1;
    }
    llvm::Module& M = *mod;

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

    // Classify each function by where it is *defined* in the input program:
    //  - body-less declarations are imported library symbols (libc/libstdc++);
    //  - definitions under the application's source tree are the program's own
    //    code, definitions elsewhere (libstdc++/glibc headers and sources) are
    //    library code;
    //  - the application tree itself comes from the compile units.
    // This must run BEFORE SVF builds its model: SVF's ExtAPI materializes stub
    // bodies for modelled C library functions (strrchr, strchr, ...), after
    // which an imported declaration would look like a definition.
    const FunctionFilter filter = FunctionFilter::build(M);

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

    // 5. Write the report (header + one #entry block per entry, spans streamed
    //    during the traversal).
    writeCallGraphReport(M, config, callGraph, filter);

    // 6. Cleanup
    delete pta;
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();

    return 0;
}
