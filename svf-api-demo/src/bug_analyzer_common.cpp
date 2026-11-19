#include "bug_analyzer_plugin.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace buganalyzer {

std::string displayFunctionName(const llvm::Function& function) {
    return llvm::demangle(function.getName().str());
}

std::string simpleFunctionName(const llvm::Function& function) {
    std::string name = displayFunctionName(function);
    const std::size_t signature = name.find('(');
    if (signature != std::string::npos)
        name.resize(signature);
    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos)
        name.erase(0, scope + 2);
    return name;
}

void setLocation(Finding& finding, const llvm::Function& function,
                 const llvm::Instruction& instruction) {
    const llvm::DebugLoc& debugLocation = instruction.getDebugLoc();
    if (debugLocation) {
        finding.line = debugLocation.getLine();
        finding.startLine = debugLocation.getLine();
        finding.endLine = debugLocation.getLine();
        finding.column = debugLocation.getCol();
        const auto* scope =
            llvm::dyn_cast<llvm::DIScope>(debugLocation.getScope());
        const llvm::DIFile* sourceFile = scope ? scope->getFile() : nullptr;
        if (!sourceFile)
            return;

        const std::string filename = sourceFile->getFilename().str();
        const std::string directory = sourceFile->getDirectory().str();
        if (!filename.empty() && filename.front() == '/')
            finding.file = filename;
        else if (!directory.empty())
            finding.file = directory + "/" + filename;
        else
            finding.file = filename;
        return;
    }

    const llvm::DISubprogram* subprogram = function.getSubprogram();
    if (!subprogram)
        return;
    finding.line = subprogram->getLine();
    finding.startLine = subprogram->getLine();
    finding.endLine = subprogram->getLine();
    const llvm::DIFile* sourceFile = subprogram->getFile();
    if (!sourceFile)
        return;
    const std::string filename = sourceFile->getFilename().str();
    const std::string directory = sourceFile->getDirectory().str();
    if (!filename.empty() && filename.front() == '/')
        finding.file = filename;
    else if (!directory.empty())
        finding.file = directory + "/" + filename;
    else
        finding.file = filename;
}

bool isNullPointer(const llvm::Value* value) {
    value = value->stripPointerCasts();
    const auto* constant = llvm::dyn_cast<llvm::Constant>(value);
    return constant && constant->getType()->isPointerTy() &&
           constant->isNullValue();
}

void addFinding(std::vector<Finding>& findings, const std::string& input,
                const llvm::Function& function,
                const llvm::Instruction& instruction, std::string rule,
                std::string cwe, std::string severity,
                std::string category, std::string message) {
    Finding finding;
    finding.input = input;
    finding.rule = std::move(rule);
    finding.cwe = std::move(cwe);
    finding.severity = std::move(severity);
    finding.category = std::move(category);
    finding.message = std::move(message);
    finding.function = displayFunctionName(function);
    setLocation(finding, function, instruction);
    findings.push_back(std::move(finding));
}
bool isAllocationFunction(const std::string& name) {
    return name == "malloc" || name == "calloc" || name == "aligned_alloc" ||
           name == "operator new" || name == "operator new[]";
}

bool isDeallocationFunction(const std::string& name) {
    return name == "free" || name == "operator delete" ||
           name == "operator delete[]";
}

} // namespace buganalyzer
