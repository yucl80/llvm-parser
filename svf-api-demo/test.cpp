#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

#include <cstring>

using namespace SVF;

struct AnalysisConfig;
void analyzeCallGraph(llvm::Module& M, const AnalysisConfig& config);

/// Configuration for analysis modes, parsed from CLI flags.
struct AnalysisConfig {
    bool useFlowSensitive = false;
    bool useVersionedFS = false;
    bool contextSensitive = false;
    bool heapModel = false;
};

static void printUsage(const char* prog) {
    llvm::errs() << "Usage: " << prog << " [options] <bitcode-file>\n"
                 << "Options:\n"
                 << "  --fs           Use flow-sensitive analysis (FSSPARSE_WPA)\n"
                 << "  --vfs          Use versioned flow-sensitive analysis (VFS_WPA)\n"
                 << "  --cs           Enable context sensitivity\n"
                 << "  --heap-model   Enable heap object model (LocMem + ModelConsts + ModelArrays)\n"
                 << "  (default)      Use AndersenWaveDiff (flow/context-insensitive)\n";
}

static AnalysisConfig parseFlags(int argc, char** argv, int& bcIdx) {
    AnalysisConfig config;
    bcIdx = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fs") == 0) {
            config.useFlowSensitive = true;
        } else if (strcmp(argv[i], "--vfs") == 0) {
            config.useVersionedFS = true;
        } else if (strcmp(argv[i], "--cs") == 0) {
            config.contextSensitive = true;
        } else if (strcmp(argv[i], "--heap-model") == 0) {
            config.heapModel = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else {
            // Assume it's the bitcode file path
            bcIdx = i;
        }
    }

    return config;
}

int main(int argc, char** argv) {
    int bcIdx;
    AnalysisConfig config = parseFlags(argc, argv, bcIdx);

    const char* bcFile = (bcIdx > 0) ? argv[bcIdx] : "test_prog.bc";

    llvm::SMDiagnostic err;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(bcFile, err, ctx);
    if (!mod) {
        llvm::errs() << "Failed to load bitcode file: " << bcFile << "\n";
        return 1;
    }

    analyzeCallGraph(*mod, config);

    return 0;
}
