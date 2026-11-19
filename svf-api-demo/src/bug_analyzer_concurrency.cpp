#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace buganalyzer {
namespace {

struct LockId {
    const llvm::Value* base = nullptr;
    int64_t offset = 0;

    friend bool operator<(const LockId& left, const LockId& right) {
        if (left.base != right.base)
            return std::less<const llvm::Value*>()(left.base, right.base);
        return left.offset < right.offset;
    }

    friend bool operator==(const LockId& left, const LockId& right) {
        return left.base == right.base && left.offset == right.offset;
    }
};

using LockSet = std::set<LockId>;

struct LockOperation {
    LockId mutex;
    bool acquire = false;
    bool reentrant = false;
};

struct LockSummary {
    std::set<unsigned> acquireParameters;
    std::set<unsigned> releaseParameters;
};

using LockSummaries =
    std::unordered_map<const llvm::Function*, LockSummary>;

struct LockOrderEdge {
    LockId before;
    LockId after;
    const llvm::Function* function;
    const llvm::Instruction* instruction;
};

struct FunctionLockInfo {
    std::unordered_map<const llvm::Instruction*, LockSet> mustHeldBefore;
    std::unordered_map<const llvm::Instruction*, LockSet> mayHeldBefore;
    std::vector<LockOrderEdge> orderEdges;
};

LockId identifyLock(const llvm::Value* pointer,
                    const llvm::DataLayout& dataLayout) {
    int64_t offset = 0;
    const llvm::Value* base = llvm::GetPointerBaseWithConstantOffset(
        pointer, offset, dataLayout);
    return {base, offset};
}

void collectArgumentSources(
    const llvm::Function& function, const llvm::Value* value,
    std::set<const llvm::Argument*>& arguments,
    std::unordered_set<const llvm::Value*>& visited) {
    if (!value || !visited.insert(value).second)
        return;
    value = value->stripPointerCasts();
    if (const auto* argument = llvm::dyn_cast<llvm::Argument>(value)) {
        arguments.insert(argument);
        return;
    }
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value)) {
        collectArgumentSources(function, gep->getPointerOperand(), arguments,
                               visited);
        return;
    }
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        const llvm::Value* storage =
            llvm::getUnderlyingObject(load->getPointerOperand());
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (store &&
                    llvm::getUnderlyingObject(store->getPointerOperand()) ==
                        storage) {
                    collectArgumentSources(function, store->getValueOperand(),
                                           arguments, visited);
                }
            }
        }
        return;
    }
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        for (const llvm::Value* incoming : phi->incoming_values())
            collectArgumentSources(function, incoming, arguments, visited);
        return;
    }
    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        collectArgumentSources(function, select->getTrueValue(), arguments,
                               visited);
        collectArgumentSources(function, select->getFalseValue(), arguments,
                               visited);
    }
}

std::set<unsigned> argumentIndices(const llvm::Function& function,
                                   const llvm::Value* value) {
    std::set<const llvm::Argument*> arguments;
    std::unordered_set<const llvm::Value*> visited;
    collectArgumentSources(function, value, arguments, visited);
    std::set<unsigned> indices;
    for (const llvm::Argument* argument : arguments)
        indices.insert(argument->getArgNo());
    return indices;
}

std::string lockLabel(const LockId& lock) {
    std::string name;
    if (lock.base && lock.base->hasName()) {
        name = lock.base->getName().str();
    } else if (lock.base) {
        llvm::raw_string_ostream stream(name);
        lock.base->printAsOperand(stream, false);
        stream.flush();
    } else {
        name = "<unknown>";
    }
    if (lock.offset > 0)
        name += "+" + std::to_string(lock.offset);
    else if (lock.offset < 0)
        name += std::to_string(lock.offset);
    return name;
}

bool identifyLockOperation(const llvm::CallBase& call,
                           const llvm::DataLayout& dataLayout,
                           const std::map<LockId, LockId>& guardLocks,
                           LockOperation& operation) {
    const auto* callee = llvm::dyn_cast<llvm::Function>(
        call.getCalledOperand()->stripPointerCasts());
    if (!callee || call.arg_size() == 0)
        return false;

    const std::string rawName = callee->getName().str();
    bool recognized = true;
    if (rawName.find("recursive_mutex") != std::string::npos &&
        rawName.find("recursive_timed_mutex") == std::string::npos &&
        (rawName.find("lockEv") != std::string::npos ||
         rawName.find("unlockEv") != std::string::npos)) {
        operation.acquire = rawName.find("lockEv") != std::string::npos &&
                            rawName.find("unlockEv") == std::string::npos;
        operation.reentrant = true;
        operation.mutex = identifyLock(call.getArgOperand(0), dataLayout);
        return operation.mutex.base &&
               !isNullPointer(call.getArgOperand(0));
    } else if (rawName == "pthread_mutex_lock" ||
        rawName == "pthread_mutex_timedlock" ||
        rawName == "pthread_mutex_trylock" ||
        rawName == "pthread_rwlock_rdlock" ||
        rawName == "pthread_rwlock_wrlock" ||
        rawName == "pthread_spin_lock") {
        operation.acquire = true;
    } else if (rawName == "pthread_mutex_unlock" ||
               rawName == "pthread_rwlock_unlock" ||
               rawName == "pthread_spin_unlock") {
        operation.acquire = false;
    } else {
        const std::string demangled = llvm::demangle(rawName);
        const bool lockGuardMethod =
            demangled.find("std::lock_guard<") != std::string::npos;
        const bool uniqueLockMethod =
            demangled.find("std::unique_lock<") != std::string::npos;
        const bool guardMethod = lockGuardMethod || uniqueLockMethod;
        const bool guardDestructor =
            (lockGuardMethod &&
             demangled.find("::~lock_guard()") != std::string::npos) ||
            (uniqueLockMethod &&
             demangled.find("::~unique_lock()") != std::string::npos);
        const bool guardConstructor =
            (lockGuardMethod &&
             demangled.find("::lock_guard(") != std::string::npos) ||
            (uniqueLockMethod &&
             demangled.find("::unique_lock(") != std::string::npos);
        if (guardMethod && guardDestructor) {
            const auto guard = guardLocks.find(
                identifyLock(call.getArgOperand(0), dataLayout));
            if (guard == guardLocks.end())
                return false;
            operation.acquire = false;
            operation.mutex = guard->second;
            return operation.mutex.base;
        }
        if (guardMethod && guardConstructor &&
            call.arg_size() > 1) {
            operation.acquire = true;
            operation.mutex =
                identifyLock(call.getArgOperand(1), dataLayout);
            return operation.mutex.base &&
                   !isNullPointer(call.getArgOperand(1));
        }
        if (uniqueLockMethod &&
            (demangled.find("::unlock()") != std::string::npos ||
             demangled.find("::lock()") != std::string::npos)) {
            const auto guard = guardLocks.find(
                identifyLock(call.getArgOperand(0), dataLayout));
            if (guard == guardLocks.end())
                return false;
            operation.acquire =
                demangled.find("::lock()") != std::string::npos;
            operation.mutex = guard->second;
            return operation.mutex.base;
        }
        const bool recursiveMutexMethod =
            (demangled.find("recursive_mutex::lock") != std::string::npos ||
             demangled.find("recursive_mutex::unlock") != std::string::npos ||
             demangled.find("recursive_timed_mutex::lock") !=
                 std::string::npos ||
             demangled.find("recursive_timed_mutex::unlock") !=
                 std::string::npos);
        if (recursiveMutexMethod) {
            operation.acquire =
                demangled.find("::lock") != std::string::npos;
            operation.reentrant = true;
            operation.mutex =
                identifyLock(call.getArgOperand(0), dataLayout);
            return operation.mutex.base &&
                   !isNullPointer(call.getArgOperand(0));
        }
        const bool mutexMethod =
            demangled.find("mutex::lock(") != std::string::npos ||
            demangled.find("mutex::unlock(") != std::string::npos ||
            demangled.find("mutex::lock_shared(") != std::string::npos ||
            demangled.find("mutex::unlock_shared(") != std::string::npos;
        if (!mutexMethod) {
            recognized = false;
        } else {
            operation.acquire =
                demangled.find("::lock(") != std::string::npos ||
                demangled.find("::lock_shared(") != std::string::npos;
            operation.reentrant =
                demangled.find("recursive_mutex::") != std::string::npos ||
                demangled.find("recursive_timed_mutex::") != std::string::npos;
        }
    }
    if (!recognized)
        return false;

    operation.mutex = identifyLock(call.getArgOperand(0), dataLayout);
    return operation.mutex.base &&
           !isNullPointer(call.getArgOperand(0));
}

using LockOperations =
    std::unordered_map<const llvm::Instruction*, LockOperation>;

using CallLockOperations =
    std::unordered_map<const llvm::Instruction*, std::vector<LockOperation>>;

LockSet transferLocks(const llvm::BasicBlock& block, LockSet locks,
                      const LockOperations& operations,
                      const CallLockOperations& callOperations) {
    for (const llvm::Instruction& instruction : block) {
        const auto found = operations.find(&instruction);
        if (found != operations.end()) {
            if (found->second.acquire)
                locks.insert(found->second.mutex);
            else
                locks.erase(found->second.mutex);
        }
        const auto calls = callOperations.find(&instruction);
        if (calls != callOperations.end()) {
            for (const LockOperation& operation : calls->second) {
                if (operation.acquire)
                    locks.insert(operation.mutex);
                else
                    locks.erase(operation.mutex);
            }
        }
    }
    return locks;
}

LockSummaries buildLockSummaries(const llvm::Module& module) {
    LockSummaries summaries;
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;
        summaries.emplace(&function, LockSummary{});
    }

    bool changed;
    do {
        changed = false;
        for (const llvm::Function& function : module) {
            if (function.isDeclaration())
                continue;
            LockSummary& summary = summaries.at(&function);

            std::map<LockId, LockId> guardLocks;
            for (const llvm::BasicBlock& block : function) {
                for (const llvm::Instruction& instruction : block) {
                    const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call || call->arg_size() < 2)
                        continue;
                    const auto* callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    if (!callee)
                        continue;
                    const std::string demangled =
                        llvm::demangle(callee->getName().str());
                    if ((demangled.find("std::lock_guard<") !=
                             std::string::npos ||
                         demangled.find("std::unique_lock<") !=
                             std::string::npos) &&
                        demangled.find("::") != std::string::npos &&
                        call->arg_size() == 2) {
                        guardLocks[identifyLock(call->getArgOperand(0),
                                                module.getDataLayout())] =
                            identifyLock(call->getArgOperand(1),
                                         module.getDataLayout());
                    }
                }
            }

            for (const llvm::BasicBlock& block : function) {
                for (const llvm::Instruction& instruction : block) {
                    const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call)
                        continue;

                    LockOperation operation;
                    if (identifyLockOperation(*call, module.getDataLayout(),
                                              guardLocks, operation)) {
                        for (unsigned index : argumentIndices(
                                 function, operation.mutex.base)) {
                            if (operation.acquire)
                                changed |= summary.acquireParameters
                                               .insert(index)
                                               .second;
                            else
                                changed |= summary.releaseParameters
                                               .insert(index)
                                               .second;
                        }
                    }

                    const auto* callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    if (!callee)
                        continue;
                    const auto found = summaries.find(callee);
                    if (found == summaries.end())
                        continue;
                    for (unsigned index : found->second.acquireParameters) {
                        if (index >= call->arg_size())
                            continue;
                        for (unsigned callerIndex : argumentIndices(
                                 function, call->getArgOperand(index)))
                            changed |= summary.acquireParameters
                                           .insert(callerIndex)
                                           .second;
                    }
                    for (unsigned index : found->second.releaseParameters) {
                        if (index >= call->arg_size())
                            continue;
                        for (unsigned callerIndex : argumentIndices(
                                 function, call->getArgOperand(index)))
                            changed |= summary.releaseParameters
                                           .insert(callerIndex)
                                           .second;
                    }
                }
            }
        }
    } while (changed);
    return summaries;
}

void intersectInto(LockSet& destination, const LockSet& source) {
    LockSet result;
    std::set_intersection(destination.begin(), destination.end(),
                          source.begin(), source.end(),
                          std::inserter(result, result.end()));
    destination.swap(result);
}

FunctionLockInfo analyzeLocks(const llvm::Function& function,
                              const llvm::DataLayout& dataLayout,
                              const std::string& input,
                              const LockSummaries& summaries,
                              std::vector<Finding>& findings) {
    std::map<LockId, LockId> guardLocks;
    for (const llvm::BasicBlock& block : function) {
        for (const llvm::Instruction& instruction : block) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call || call->arg_size() < 2)
                continue;
            const auto* callee = llvm::dyn_cast<llvm::Function>(
                call->getCalledOperand()->stripPointerCasts());
            if (!callee)
                continue;
            const std::string demangled =
                llvm::demangle(callee->getName().str());
            const bool lockGuardConstructor =
                demangled.find("std::lock_guard<") != std::string::npos &&
                demangled.find("::lock_guard(") != std::string::npos;
            const bool uniqueLockConstructor =
                demangled.find("std::unique_lock<") != std::string::npos &&
                demangled.find("::unique_lock(") != std::string::npos;
            if ((lockGuardConstructor || uniqueLockConstructor) &&
                call->arg_size() == 2) {
                guardLocks[identifyLock(call->getArgOperand(0), dataLayout)] =
                    identifyLock(call->getArgOperand(1), dataLayout);
            }
        }
    }

    LockOperations operations;
    CallLockOperations callOperations;
    LockSet universe;
    for (const llvm::BasicBlock& block : function) {
        for (const llvm::Instruction& instruction : block) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call)
                continue;
            LockOperation operation;
            if (identifyLockOperation(*call, dataLayout, guardLocks,
                                      operation)) {
                operations.emplace(&instruction, operation);
                universe.insert(operation.mutex);
                continue;
            }
            const auto* callee = llvm::dyn_cast<llvm::Function>(
                call->getCalledOperand()->stripPointerCasts());
            if (!callee)
                continue;
            const auto summary = summaries.find(callee);
            if (summary == summaries.end())
                continue;
            std::vector<LockOperation> calledOperations;
            for (unsigned index : summary->second.acquireParameters) {
                if (index < call->arg_size()) {
                    LockOperation called;
                    called.acquire = true;
                    called.mutex =
                        identifyLock(call->getArgOperand(index), dataLayout);
                    if (called.mutex.base)
                        calledOperations.push_back(called);
                }
            }
            for (unsigned index : summary->second.releaseParameters) {
                if (index < call->arg_size()) {
                    LockOperation called;
                    called.acquire = false;
                    called.mutex =
                        identifyLock(call->getArgOperand(index), dataLayout);
                    if (called.mutex.base)
                        calledOperations.push_back(called);
                }
            }
            if (!calledOperations.empty()) {
                for (const LockOperation& called : calledOperations)
                    universe.insert(called.mutex);
                callOperations.emplace(&instruction,
                                       std::move(calledOperations));
            }
        }
    }

    std::unordered_map<const llvm::BasicBlock*, LockSet> mustIn, mustOut;
    std::unordered_map<const llvm::BasicBlock*, LockSet> mayIn, mayOut;
    const llvm::BasicBlock* entry = &function.getEntryBlock();
    for (const llvm::BasicBlock& block : function) {
        mustIn[&block] = &block == entry ? LockSet{} : universe;
        mustOut[&block] = &block == entry ? LockSet{} : universe;
        mayIn[&block] = {};
        mayOut[&block] = {};
    }

    bool changed;
    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            LockSet nextMustIn;
            LockSet nextMayIn;
            if (&block != entry) {
                bool firstPredecessor = true;
                for (const llvm::BasicBlock* predecessor :
                     llvm::predecessors(&block)) {
                    if (firstPredecessor) {
                        nextMustIn = mustOut[predecessor];
                        firstPredecessor = false;
                    } else {
                        intersectInto(nextMustIn, mustOut[predecessor]);
                    }
                    nextMayIn.insert(mayOut[predecessor].begin(),
                                     mayOut[predecessor].end());
                }
            }
            LockSet nextMustOut =
                transferLocks(block, nextMustIn, operations,
                              callOperations);
            LockSet nextMayOut =
                transferLocks(block, nextMayIn, operations,
                              callOperations);
            if (mustIn[&block] != nextMustIn ||
                mustOut[&block] != nextMustOut ||
                mayIn[&block] != nextMayIn ||
                mayOut[&block] != nextMayOut) {
                mustIn[&block] = std::move(nextMustIn);
                mustOut[&block] = std::move(nextMustOut);
                mayIn[&block] = std::move(nextMayIn);
                mayOut[&block] = std::move(nextMayOut);
                changed = true;
            }
        }
    } while (changed);

    FunctionLockInfo info;
    for (const llvm::BasicBlock& block : function) {
        LockSet mustHeld = mustIn[&block];
        LockSet mayHeld = mayIn[&block];
        for (const llvm::Instruction& instruction : block) {
            info.mustHeldBefore[&instruction] = mustHeld;
            info.mayHeldBefore[&instruction] = mayHeld;

            std::vector<LockOperation> operationsAtInstruction;
            const auto direct = operations.find(&instruction);
            if (direct != operations.end())
                operationsAtInstruction.push_back(direct->second);
            const auto called = callOperations.find(&instruction);
            if (called != callOperations.end())
                operationsAtInstruction.insert(
                    operationsAtInstruction.end(),
                    called->second.begin(), called->second.end());

            for (const LockOperation& operation : operationsAtInstruction) {
                const LockId& mutex = operation.mutex;
                if (operation.acquire) {
                    if (mayHeld.count(mutex) != 0 &&
                        !operation.reentrant) {
                        addFinding(findings, input, function, instruction,
                                   "SELF_DEADLOCK", "CWE-833", "medium",
                                   "concurrency",
                                   "The same mutex may be acquired twice "
                                   "without an intervening unlock; this can "
                                   "self-deadlock for non-recursive mutexes: " +
                                       lockLabel(mutex) + ".");
                    }
                    for (const LockId& held : mayHeld) {
                        if (!(held == mutex))
                            info.orderEdges.push_back(
                                {held, mutex, &function, &instruction});
                    }
                    mustHeld.insert(mutex);
                    mayHeld.insert(mutex);
                } else {
                    bool formalParameter = false;
                    for (const llvm::Argument& argument : function.args()) {
                        if (identifyLock(&argument, dataLayout) == mutex) {
                            formalParameter = true;
                            break;
                        }
                    }
                    if (mayHeld.count(mutex) == 0 && !formalParameter) {
                        addFinding(findings, input, function, instruction,
                                   "UNLOCK_WITHOUT_LOCK", "CWE-667",
                                   "medium", "concurrency",
                                   "Mutex is unlocked without a recognized "
                                   "prior lock in this function: " +
                                       lockLabel(mutex) + ".");
                    } else if (mustHeld.count(mutex) == 0) {
                        addFinding(findings, input, function, instruction,
                                   "UNBALANCED_UNLOCK", "CWE-667", "medium",
                                   "concurrency",
                                   "Mutex may be unlocked on a path where it "
                                   "was not locked: " + lockLabel(mutex) + ".");
                    }
                    mustHeld.erase(mutex);
                    mayHeld.erase(mutex);
                }
            }

            if (llvm::isa<llvm::ReturnInst>(instruction) ||
                llvm::isa<llvm::ResumeInst>(instruction)) {
                for (const LockId& held : mayHeld) {
                    addFinding(findings, input, function, instruction,
                               "LOCK_NOT_RELEASED", "CWE-667", "medium",
                               "concurrency",
                               "Function may return while holding mutex " +
                                   lockLabel(held) + ".");
                }
            }
        }
    }
    return info;
}

bool hasLockPath(const LockId& start, const LockId& target,
                 const std::vector<LockOrderEdge>& edges) {
    std::map<LockId, std::vector<LockId>> graph;
    for (const LockOrderEdge& edge : edges)
        graph[edge.before].push_back(edge.after);

    std::vector<LockId> pending{start};
    LockSet visited;
    while (!pending.empty()) {
        const LockId current = pending.back();
        pending.pop_back();
        if (current == target)
            return true;
        if (!visited.insert(current).second)
            continue;
        const auto found = graph.find(current);
        if (found != graph.end())
            pending.insert(pending.end(), found->second.begin(),
                           found->second.end());
    }
    return false;
}

void reportDeadlockCycles(const std::string& input,
                          const std::vector<LockOrderEdge>& edges,
                          std::vector<Finding>& findings) {
    std::set<std::pair<LockId, LockId>> reported;
    for (const LockOrderEdge& edge : edges) {
        if (!(edge.before == edge.after) &&
            hasLockPath(edge.after, edge.before, edges) &&
            reported.emplace(edge.before, edge.after).second) {
            addFinding(findings, input, *edge.function, *edge.instruction,
                       "LOCK_ORDER_CYCLE", "CWE-833", "high", "concurrency",
                       "Conflicting lock order " + lockLabel(edge.before) +
                           " -> " + lockLabel(edge.after) +
                           " participates in a possible deadlock cycle.");
        }
    }
}

std::vector<const llvm::Function*>
findThreadEntryFunctions(const llvm::Module& module) {
    std::vector<const llvm::Function*> entries;
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call)
                    continue;
                const auto* callee = llvm::dyn_cast<llvm::Function>(
                    call->getCalledOperand()->stripPointerCasts());
                if (!callee)
                    continue;
                const std::string rawName = callee->getName().str();
                const std::string demangled = llvm::demangle(rawName);
                if (rawName == "pthread_create" && call->arg_size() > 2) {
                    const auto* start = llvm::dyn_cast<llvm::Function>(
                        call->getArgOperand(2)->stripPointerCasts());
                    if (start && !start->isDeclaration())
                        entries.push_back(start);
                } else if ((rawName == "CreateThread" ||
                            rawName == "CreateRemoteThread") &&
                           call->arg_size() > 2) {
                    const auto* start = llvm::dyn_cast<llvm::Function>(
                        call->getArgOperand(2)->stripPointerCasts());
                    if (start && !start->isDeclaration())
                        entries.push_back(start);
                } else if (rawName == "_beginthread" ||
                           rawName == "_beginthreadex") {
                    const unsigned startIndex =
                        rawName == "_beginthreadex" ? 2 : 0;
                    if (call->arg_size() > startIndex) {
                        const auto* start = llvm::dyn_cast<llvm::Function>(
                            call->getArgOperand(startIndex)
                                ->stripPointerCasts());
                        if (start && !start->isDeclaration())
                            entries.push_back(start);
                    }
                } else if (demangled.find("std::thread::thread") !=
                           std::string::npos) {
                    for (unsigned i = 1; i < call->arg_size(); ++i) {
                        const auto* start = llvm::dyn_cast<llvm::Function>(
                            call->getArgOperand(i)->stripPointerCasts());
                        if (start && !start->isDeclaration())
                            entries.push_back(start);
                    }
                } else if (demangled.find("std::jthread::jthread") !=
                           std::string::npos) {
                    for (unsigned i = 1; i < call->arg_size(); ++i) {
                        const auto* start = llvm::dyn_cast<llvm::Function>(
                            call->getArgOperand(i)->stripPointerCasts());
                        if (start && !start->isDeclaration())
                            entries.push_back(start);
                    }
                } else if (demangled.find("std::async<") !=
                           std::string::npos) {
                    for (unsigned i = 1; i < call->arg_size(); ++i) {
                        const auto* start = llvm::dyn_cast<llvm::Function>(
                            call->getArgOperand(i)->stripPointerCasts());
                        if (start && !start->isDeclaration())
                            entries.push_back(start);
                    }
                }
            }
        }
    }
    return entries;
}

std::vector<const llvm::Function*>
reachableFunctions(const llvm::Function& entry) {
    std::vector<const llvm::Function*> result;
    std::vector<const llvm::Function*> pending{&entry};
    std::unordered_set<const llvm::Function*> visited;
    while (!pending.empty()) {
        const llvm::Function* function = pending.back();
        pending.pop_back();
        if (!visited.insert(function).second)
            continue;
        result.push_back(function);
        for (const llvm::BasicBlock& block : *function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call)
                    continue;
                const auto* callee = llvm::dyn_cast<llvm::Function>(
                    call->getCalledOperand()->stripPointerCasts());
                if (callee && !callee->isDeclaration())
                    pending.push_back(callee);
            }
        }
    }
    return result;
}

struct GlobalAccess {
    const llvm::Value* memory;
    const llvm::Function* function;
    const llvm::Instruction* instruction;
    bool writes;
    bool atomic;
    int64_t offset;
    uint64_t size;
    LockSet mustHeld;
};

using ThreadAccesses =
    std::map<const llvm::Value*, std::vector<GlobalAccess>>;

ThreadAccesses collectGlobalAccesses(
    const llvm::Function& entry,
    const llvm::DataLayout& dataLayout,
    const std::unordered_map<const llvm::Function*, FunctionLockInfo>& lockInfo) {
    ThreadAccesses accesses;
    for (const llvm::Function* function : reachableFunctions(entry)) {
        const auto state = lockInfo.find(function);
        for (const llvm::BasicBlock& block : *function) {
            for (const llvm::Instruction& instruction : block) {
                const llvm::Value* pointer = nullptr;
                bool writes = false;
                bool atomic = false;
                if (const auto* load =
                        llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    pointer = load->getPointerOperand();
                    atomic = load->isAtomic();
                } else if (const auto* store =
                               llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    pointer = store->getPointerOperand();
                    writes = true;
                    atomic = store->isAtomic();
                } else if (const auto* atomicRmw =
                               llvm::dyn_cast<llvm::AtomicRMWInst>(
                                   &instruction)) {
                    pointer = atomicRmw->getPointerOperand();
                    writes = true;
                    atomic = true;
                } else if (const auto* atomicCmpXchg =
                               llvm::dyn_cast<llvm::AtomicCmpXchgInst>(
                                   &instruction)) {
                    pointer = atomicCmpXchg->getPointerOperand();
                    writes = true;
                    atomic = true;
                }
                if (!pointer)
                    continue;
                int64_t offset = 0;
                const llvm::Value* pointerBase =
                    llvm::GetPointerBaseWithConstantOffset(
                        pointer, offset, dataLayout);
                const llvm::Value* memory =
                    llvm::getUnderlyingObject(pointerBase);
                if (!memory)
                    continue;
                if (const auto* global =
                        llvm::dyn_cast<llvm::GlobalVariable>(memory)) {
                    if (global->isConstant() || global->isThreadLocal())
                        continue;
                }
                if (llvm::isa<llvm::AllocaInst>(memory))
                    continue;
                llvm::Type* accessedType =
                    instruction.getType()->isVoidTy()
                        ? nullptr
                        : instruction.getType();
                if (const auto* load =
                        llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    accessedType = load->getType();
                else if (const auto* store =
                             llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    accessedType = store->getValueOperand()->getType();
                else if (const auto* atomicRmw =
                             llvm::dyn_cast<llvm::AtomicRMWInst>(
                                 &instruction))
                    accessedType = atomicRmw->getValOperand()->getType();
                else if (const auto* atomicCmpXchg =
                             llvm::dyn_cast<llvm::AtomicCmpXchgInst>(
                                 &instruction))
                    accessedType =
                        atomicCmpXchg->getCompareOperand()->getType();
                if (!accessedType)
                    continue;
                const uint64_t size =
                    dataLayout.getTypeStoreSize(accessedType)
                        .getKnownMinValue();
                LockSet held;
                if (state != lockInfo.end()) {
                    const auto locks = state->second.mustHeldBefore.find(
                        &instruction);
                    if (locks != state->second.mustHeldBefore.end())
                        held = locks->second;
                }
                accesses[memory].push_back(
                    {memory, function, &instruction, writes, atomic, offset,
                     size, std::move(held)});
            }
        }
    }
    return accesses;
}

bool shareRecognizedLock(const LockSet& left, const LockSet& right) {
    for (const LockId& mutex : left) {
        if (right.count(mutex) != 0)
            return true;
    }
    return false;
}

bool accessesOverlap(const GlobalAccess& left, const GlobalAccess& right) {
    if (left.offset <= right.offset)
        return static_cast<uint64_t>(right.offset - left.offset) < left.size;
    return static_cast<uint64_t>(left.offset - right.offset) < right.size;
}

void reportPotentialDataRaces(
    const llvm::Module& module, const std::string& input,
    const std::unordered_map<const llvm::Function*, FunctionLockInfo>& lockInfo,
    std::vector<Finding>& findings) {
    std::vector<const llvm::Function*> entries =
        findThreadEntryFunctions(module);
    const llvm::Function* main = module.getFunction("main");
    if (main && !main->isDeclaration())
        entries.insert(entries.begin(), main);
    if (entries.size() < 2)
        return;

    std::vector<ThreadAccesses> accesses;
    accesses.reserve(entries.size());
    for (const llvm::Function* entry : entries)
        accesses.push_back(
            collectGlobalAccesses(*entry, module.getDataLayout(), lockInfo));

    std::set<std::tuple<std::size_t, std::size_t,
                        const llvm::Value*>> reported;
    for (std::size_t i = 0; i < accesses.size(); ++i) {
        for (std::size_t j = i + 1; j < accesses.size(); ++j) {
            for (const auto& globalAccesses : accesses[i]) {
                const auto other = accesses[j].find(globalAccesses.first);
                if (other == accesses[j].end())
                    continue;
                bool reportedPair = false;
                for (const GlobalAccess& left : globalAccesses.second) {
                    for (const GlobalAccess& right : other->second) {
                        if ((!left.writes && !right.writes) ||
                            (left.atomic && right.atomic) ||
                            !accessesOverlap(left, right) ||
                            shareRecognizedLock(left.mustHeld,
                                                right.mustHeld))
                            continue;
                        if (!reported.emplace(i, j, globalAccesses.first)
                                 .second)
                            continue;

                        std::string memoryName;
                        if (const auto* global =
                                llvm::dyn_cast<llvm::GlobalVariable>(
                                    globalAccesses.first)) {
                            memoryName = global->getName().str();
                        } else if (globalAccesses.first->hasName()) {
                            memoryName =
                                globalAccesses.first->getName().str();
                        } else {
                            llvm::raw_string_ostream stream(memoryName);
                            globalAccesses.first->printAsOperand(stream,
                                                                 false);
                            stream.flush();
                        }
                        const std::string leftEntry =
                            displayFunctionName(*entries[i]);
                        const std::string rightEntry =
                            displayFunctionName(*entries[j]);
                        addFinding(
                            findings, input, *left.function, *left.instruction,
                            "POTENTIAL_DATA_RACE", "CWE-362", "medium",
                            "concurrency",
                            "Shared memory '" + memoryName +
                                "' has conflicting non-atomic accesses from "
                                "concurrent entries '" + leftEntry + "' and '" +
                                rightEntry +
                                "' without a common recognized lock.");
                        reportedPair = true;
                        break;
                    }
                    if (reportedPair)
                        break;
                }
            }
        }
    }
}

} // namespace

void analyzeConcurrency(const llvm::Module& module, const std::string& input,
                        std::vector<Finding>& findings) {
    std::unordered_map<const llvm::Function*, FunctionLockInfo> lockInfo;
    std::vector<LockOrderEdge> lockOrders;
    const LockSummaries lockSummaries = buildLockSummaries(module);
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;
        FunctionLockInfo info =
            analyzeLocks(function, module.getDataLayout(), input,
                         lockSummaries, findings);
        lockOrders.insert(lockOrders.end(), info.orderEdges.begin(),
                          info.orderEdges.end());
        lockInfo.emplace(&function, std::move(info));
    }
    reportDeadlockCycles(input, lockOrders, findings);
    reportPotentialDataRaces(module, input, lockInfo, findings);
}

} // namespace buganalyzer
