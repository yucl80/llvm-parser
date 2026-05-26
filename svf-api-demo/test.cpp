#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

using namespace SVF;

void analyzeCallGraph(llvm::Module& M);

int main(int argc, char** argv) {
    const char* bcFile = (argc > 1) ? argv[1] : "test_prog.bc";

    llvm::SMDiagnostic err;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(bcFile, err, ctx);
    if (!mod) {
        llvm::errs() << "Failed to load bitcode file: " << bcFile << "\n";
        return 1;
    }

    analyzeCallGraph(*mod);

    return 0;
}
