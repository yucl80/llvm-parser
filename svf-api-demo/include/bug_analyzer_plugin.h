#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <string>
#include <vector>

namespace buganalyzer {

struct Finding {
    std::string input;
    std::string rule;
    std::string cwe;
    std::string severity;
    std::string confidence = "confirmed";
    std::string category;
    std::string message;
    std::string function;
    std::string file;
    unsigned line = 0;
    unsigned startLine = 0;
    unsigned endLine = 0;
    unsigned column = 0;
};

using AnalyzeFunction =
    void (*)(const llvm::Module&, const std::string&, std::vector<Finding>&);

struct AnalyzerPlugin {
    const char* name;
    AnalyzeFunction analyze;
};

const std::vector<AnalyzerPlugin>& registeredAnalyzers();
void runAnalyzers(const llvm::Module& module, const std::string& input,
                  std::vector<Finding>& findings);

std::string displayFunctionName(const llvm::Function& function);
std::string simpleFunctionName(const llvm::Function& function);
bool isNullPointer(const llvm::Value* value);
bool isAllocationFunction(const std::string& name);
bool isDeallocationFunction(const std::string& name);
void addFinding(std::vector<Finding>& findings, const std::string& input,
                const llvm::Function& function,
                const llvm::Instruction& instruction, std::string rule,
                std::string cwe, std::string severity,
                std::string category, std::string message,
                std::string confidence = "confirmed");

void analyzeMemorySafety(const llvm::Module& module, const std::string& input,
                         std::vector<Finding>& findings);
void analyzeConcurrency(const llvm::Module& module, const std::string& input,
                        std::vector<Finding>& findings);
void analyzeUninitialized(const llvm::Module& module, const std::string& input,
                          std::vector<Finding>& findings);
void analyzeUnsafeApis(const llvm::Module& module, const std::string& input,
                       std::vector<Finding>& findings);
void analyzeNestedLoops(const llvm::Module& module, const std::string& input,
                        std::vector<Finding>& findings);
void analyzeResourceLifetimes(const llvm::Module& module,
                              const std::string& input,
                              std::vector<Finding>& findings);
bool hasCrossProcessAccess(const llvm::Function& function);
bool hasLocalCrossProcessAccess(const llvm::Function& function);
bool hasRemoteCrossProcessAccess(const llvm::Function& function);
void configureCrossProcessApis(const std::vector<std::string>& apis);
void configureCrossProcessApis(const std::vector<std::string>& localApis,
                               const std::vector<std::string>& remoteApis);
void analyzeLocalCrossProcess(const llvm::Module& module,
                              const std::string& input,
                              std::vector<Finding>& findings);
void analyzeRemoteCrossProcess(const llvm::Module& module,
                               const std::string& input,
                               std::vector<Finding>& findings);

} // namespace buganalyzer
