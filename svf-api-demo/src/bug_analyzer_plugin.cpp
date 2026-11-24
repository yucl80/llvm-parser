#include "bug_analyzer_plugin.h"

namespace buganalyzer {

const std::vector<AnalyzerPlugin>& registeredAnalyzers() {
    static const std::vector<AnalyzerPlugin> plugins = {
        {"memory-safety", analyzeMemorySafety},
        {"concurrency", analyzeConcurrency},
        {"uninitialized-read", analyzeUninitialized},
        {"unsafe-api", analyzeUnsafeApis},
        {"nested-loops", analyzeNestedLoops},
        {"resource-lifetime", analyzeResourceLifetimes},
        {"cross-process-local", analyzeLocalCrossProcess},
        {"cross-process-remote", analyzeRemoteCrossProcess},
    };
    return plugins;
}

void runAnalyzers(const llvm::Module& module, const std::string& input,
                  std::vector<Finding>& findings) {
    for (const AnalyzerPlugin& plugin : registeredAnalyzers())
        plugin.analyze(module, input, findings);
}

} // namespace buganalyzer
