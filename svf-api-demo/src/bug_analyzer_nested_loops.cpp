#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include <cstdint>

namespace buganalyzer {
namespace {

constexpr std::uint64_t MaxSmallNestedIterationCount = 256;

bool hasSideEffectingCall(const llvm::Loop& loop) {
    for (const llvm::BasicBlock* block : loop.blocks()) {
        for (const llvm::Instruction& instruction : *block) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (call && call->mayHaveSideEffects())
                return true;
        }
    }
    return false;
}

std::uint64_t multiplyIterationBounds(std::uint64_t current,
                                      unsigned tripCount) {
    if (current > MaxSmallNestedIterationCount / tripCount)
        return MaxSmallNestedIterationCount + 1;
    return current * tripCount;
}

void findPotentiallyExpensiveNestedLoop(
    llvm::Loop& loop, llvm::ScalarEvolution& scalarEvolution,
    std::uint64_t enclosingIterations, bool enclosingBoundsKnown,
    llvm::Loop*& candidate, unsigned& candidateDepth) {
    const unsigned tripCount =
        scalarEvolution.getSmallConstantMaxTripCount(&loop);
    const bool boundsKnown = enclosingBoundsKnown && tripCount != 0;
    const std::uint64_t iterationBound =
        tripCount == 0
            ? enclosingIterations
            : multiplyIterationBounds(enclosingIterations, tripCount);

    if (loop.getLoopDepth() >= 2 &&
        (!boundsKnown || iterationBound > MaxSmallNestedIterationCount ||
         hasSideEffectingCall(loop)) &&
        loop.getLoopDepth() > candidateDepth) {
        candidate = &loop;
        candidateDepth = loop.getLoopDepth();
    }

    for (llvm::Loop* subLoop : loop.getSubLoops())
        findPotentiallyExpensiveNestedLoop(
            *subLoop, scalarEvolution, iterationBound, boundsKnown,
            candidate, candidateDepth);
}

const llvm::Instruction* debugInstruction(const llvm::BasicBlock& block) {
    for (const llvm::Instruction& instruction : block) {
        if (instruction.getDebugLoc())
            return &instruction;
    }
    return block.empty() ? nullptr : &block.front();
}

} // namespace

void analyzeNestedLoops(const llvm::Module& module, const std::string& input,
                        std::vector<Finding>& findings) {
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;

        llvm::DominatorTree dominatorTree(
            const_cast<llvm::Function&>(function));
        llvm::LoopInfo loopInfo(dominatorTree);
        llvm::Function& mutableFunction =
            const_cast<llvm::Function&>(function);
        llvm::TargetLibraryInfoImpl targetLibraryInfoImpl(
            llvm::Triple(function.getParent()->getTargetTriple()));
        llvm::TargetLibraryInfo targetLibraryInfo(targetLibraryInfoImpl);
        llvm::AssumptionCache assumptionCache(mutableFunction);
        llvm::ScalarEvolution scalarEvolution(
            mutableFunction, targetLibraryInfo, assumptionCache,
            dominatorTree, loopInfo);

        llvm::Loop* candidate = nullptr;
        unsigned candidateDepth = 0;
        for (llvm::Loop* loop : loopInfo)
            findPotentiallyExpensiveNestedLoop(
                *loop, scalarEvolution, 1, true, candidate, candidateDepth);

        if (!candidate)
            continue;
        const llvm::Instruction* site =
            debugInstruction(*candidate->getHeader());
        if (!site)
            continue;

        addFinding(findings, input, function, *site, "NESTED_LOOPS", "",
                   "info", "complexity",
                   "Function contains nested loops with an unknown or "
                   "greater-than-256 iteration bound or a side-effecting "
                   "inner-loop call; nesting depth: " +
                       std::to_string(candidateDepth) + ".");
    }
}

} // namespace buganalyzer
