#include "callgraph_analysis.h"

#include "callgraph_output.h"
#include "function_filter.h"

#include "WPA/Andersen.h"
#include "WPA/FlowSensitive.h"
#include "WPA/VersionedFlowSensitive.h"
#include "DDA/ContextDDA.h"
#include "DDA/FlowDDA.h"
#include "DDA/DDAClient.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>

#include <memory>
#include <vector>

using namespace SVF;

std::string analysisPtaName(const AnalysisConfig& config) {
    if (config.useDDA)
        return config.ddaFlowSensitive ? "FlowDDA" : "ContextDDA";
    if (config.useFlowSensitive)
        return "FlowSensitive";
    if (config.useVersionedFS)
        return "VersionedFlowSensitive";
    return "AndersenWaveDiff";
}

int analyzeCallGraph(const std::vector<std::string>& bcFiles,
                     const AnalysisConfig& config) {
    if (bcFiles.empty()) {
        llvm::errs() << "[ERROR] at least one bitcode file path is required\n";
        return 1;
    }

    // Keep SVF's statistics off stdout for *every* PTA this process creates —
    // including the Andersen pre-analysis, MemSSA and SVFG objects that DDA
    // builds internally during initialize() (per-PTA disablePrintStat() is too
    // late for those). Reports go to their own file, not stdout.
    Options::PStat.setValue(false);

    FunctionFilter filter;
    llvm::LLVMContext ctx;
    std::vector<std::unique_ptr<llvm::Module>> modules;
    modules.reserve(bcFiles.size());
    {
        for (const std::string& bcFile : bcFiles) {
            llvm::SMDiagnostic err;
            std::unique_ptr<llvm::Module> mod =
                llvm::parseIRFile(bcFile, err, ctx);
            if (!mod) {
                llvm::errs() << "Failed to load bitcode file: " << bcFile << "\n";
                err.print("clang_analyzer", llvm::errs());
                return 1;
            }
            modules.push_back(std::move(mod));
        }
    }

    // Merge in memory so declarations in one module resolve to definitions in
    // another. This avoids writing a large linked bitcode file to disk.
    std::unique_ptr<llvm::Module> linkedModule = std::move(modules.front());
    for (size_t i = 1; i < modules.size(); ++i) {
        if (llvm::Linker::linkModules(*linkedModule, std::move(modules[i]))) {
            llvm::errs() << "[ERROR] failed to link bitcode module: "
                         << bcFiles[i] << "\n";
            return 1;
        }
    }
    // Classify after linking so internal symbols renamed by the linker retain
    // their debug locations and definition-site classification. This still
    // runs before SVF adds ExtAPI stub bodies.
    filter = FunctionFilter::build(*linkedModule, config.nameFilters,
                                   config.appSourceRoots);

    llvm::errs() << "[Config] pta=" << analysisPtaName(config)
                 << " modules=" << bcFiles.size()
                 << " context_sensitive=" << (config.contextSensitive ? "true" : "false")
                 << " heap_model=" << (config.heapModel ? "true" : "false")
                 << " exclude_std=" << (config.excludeStd ? "true" : "false")
                 << " name_filters=" << config.nameFilters.size() << "\n";

    if (config.heapModel) {
        Options::ModelConsts.setValue(true);
        Options::ModelArrays.setValue(true);
        llvm::errs() << "[Config] Heap object model: ModelConsts=true, ModelArrays=true\n";
    }

    // Build SVF's model from the in-memory linked module.
    LLVMModuleSet::buildSVFModule(*linkedModule);
    llvm::Module& M = *linkedModule;

    // 2. Build SVFIR
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    // 3. Select and run pointer analysis. The static create* helpers run
    //    analyze() during construction, before disablePrintStat() could take
    //    effect, so we construct manually to keep SVF's statistics off stdout.
    PointerAnalysis* pta = nullptr;
    std::unique_ptr<DDAClient> ddaClient;
    if (config.useDDA) {
        // Demand-driven value-flow analysis: no full points-to solve. A
        // FunptrDDAClient restricts the queries to the (v)function-pointer
        // variables feeding the module's indirect callsites; the DDA engine
        // answers them backwards on the SVFG (ContextDDA: context-sensitive,
        // FlowDDA: flow-sensitive), falling back to a lightweight Andersen
        // pre-analysis only on out-of-budget paths. The indirect calls are
        // then resolved from the on-demand points-to sets below.
        ddaClient = std::make_unique<FunptrDDAClient>();
        if (config.ddaOptSvfg) {
            // Must be set BEFORE the DDA object is constructed: SVFGBuilder
            // snapshots Options::OPTSVFG() in its constructor and then chooses
            // between the plain pointer-only SVFG and the optimised SVFGOPT.
            Options::OPTSVFG.setValue(true);
            llvm::errs() << "[Config] dda opt_svfg=true\n";
        }
        // Mirror DDAPass: propagate the path/context limits into the DP-item
        // machinery (static state, must be set before the first query).
        ContextCond::setMaxPathLen(Options::MaxPathLen());
        ContextCond::setMaxCxtLen(config.ddaContextLen);
        llvm::errs() << "[Config] dda cxt_len=" << config.ddaContextLen << "\n";
        if (config.ddaFlowSensitive)
            pta = new FlowDDA(pag, ddaClient.get());
        else
            pta = new ContextDDA(pag, ddaClient.get());
        pta->disablePrintStat();
        pta->initialize();
        ddaClient->answerQueries(pta);
        // finalize() normalizes the on-demand (context-conditional) points-to
        // sets into the bit-vector representation that getPts(NodeID) — used
        // by the resolution loop below — requires (CondPTAImpl asserts on it).
        pta->finalize();

        // Resolve every indirect callsite from the queried points-to sets
        // (same logic as BVDataPTAImpl::onTheFlyCallGraphSolve), turning the
        // on-demand results into call-graph edges.
        for (const auto& it : pag->getIndirectCallsites()) {
            const CallICFGNode* cs = it.first;
            if (cs->isVirtualCall()) {
                const SVFVar* vtbl = cs->getVtablePtr();
                pta->resolveCPPIndCalls(cs, pta->getPts(vtbl->getId()),
                                        pta->getIndCallMap());
            } else {
                pta->resolveIndCalls(cs, pta->getPts(it.second),
                                     pta->getIndCallMap());
            }
        }
    } else {
        if (config.useFlowSensitive) {
            pta = new FlowSensitive(pag);
        } else if (config.useVersionedFS) {
            pta = new VersionedFlowSensitive(pag);
        } else {
            pta = new AndersenWaveDiff(pag, PTATY::AndersenWaveDiff_WPA, false);
        }
        pta->disablePrintStat();
        pta->analyze();
    }

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
