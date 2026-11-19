#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace buganalyzer {
namespace {

bool hasOutOfBoundsIndex(const llvm::GetElementPtrInst& gep) {
    llvm::Type* indexedType = gep.getSourceElementType();
    bool firstIndex = true;
    for (const llvm::Use& indexUse : gep.indices()) {
        const llvm::Value* index = indexUse.get();
        if (firstIndex) {
            firstIndex = false;
            continue;
        }

        const auto* constantIndex = llvm::dyn_cast<llvm::ConstantInt>(index);
        if (constantIndex) {
            const llvm::APInt& value = constantIndex->getValue();
            if (const auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(
                    indexedType)) {
                if (value.isNegative() ||
                    value.getLimitedValue() >= arrayType->getNumElements())
                    return true;
            } else if (const auto* structType =
                           llvm::dyn_cast<llvm::StructType>(indexedType)) {
                if (value.isNegative() ||
                    value.getLimitedValue() >= structType->getNumElements())
                    return true;
            } else if (const auto* vectorType =
                           llvm::dyn_cast<llvm::FixedVectorType>(indexedType)) {
                if (value.isNegative() ||
                    value.getLimitedValue() >= vectorType->getNumElements())
                    return true;
            }
        }

        llvm::Type* nextType = llvm::GetElementPtrInst::getTypeAtIndex(
            indexedType, const_cast<llvm::Value*>(index));
        if (!nextType)
            return false;
        indexedType = nextType;
    }
    return false;
}

bool hasOnlyFlexibleArrayOutOfBoundsIndex(
    const llvm::GetElementPtrInst& gep) {
    llvm::Type* indexedType = gep.getSourceElementType();
    bool firstIndex = true;
    bool foundFlexibleArrayIndex = false;
    for (const llvm::Use& indexUse : gep.indices()) {
        const llvm::Value* index = indexUse.get();
        if (firstIndex) {
            firstIndex = false;
        } else if (const auto* constantIndex =
                       llvm::dyn_cast<llvm::ConstantInt>(index)) {
            const llvm::APInt& value = constantIndex->getValue();
            bool outOfBounds = false;
            bool flexibleArray = false;
            if (const auto* arrayType =
                    llvm::dyn_cast<llvm::ArrayType>(indexedType)) {
                outOfBounds =
                    value.isNegative() ||
                    value.getLimitedValue() >= arrayType->getNumElements();
                flexibleArray = outOfBounds &&
                                arrayType->getNumElements() == 0 &&
                                !value.isNegative();
            } else if (const auto* structType =
                           llvm::dyn_cast<llvm::StructType>(indexedType)) {
                outOfBounds =
                    value.isNegative() ||
                    value.getLimitedValue() >= structType->getNumElements();
            } else if (const auto* vectorType =
                           llvm::dyn_cast<llvm::FixedVectorType>(indexedType)) {
                outOfBounds =
                    value.isNegative() ||
                    value.getLimitedValue() >= vectorType->getNumElements();
            }

            if (outOfBounds && !flexibleArray)
                return false;
            foundFlexibleArrayIndex |= flexibleArray;
        }

        llvm::Type* nextType = llvm::GetElementPtrInst::getTypeAtIndex(
            indexedType, const_cast<llvm::Value*>(index));
        if (!nextType)
            return false;
        indexedType = nextType;
    }
    return foundFlexibleArrayIndex;
}

using PointerAliases =
    std::unordered_map<const llvm::Value*, const llvm::Value*>;
using ReachableBlocks = std::unordered_set<const llvm::BasicBlock*>;

std::string memorySafetyFunctionName(const llvm::Function& function) {
    std::string name = displayFunctionName(function);
    const std::size_t signature = name.find('(');
    if (signature != std::string::npos)
        name.resize(signature);
    return name;
}

bool isMemorySafetyAllocationFunction(const llvm::Function& function) {
    const std::string name = memorySafetyFunctionName(function);
    return name == "malloc" || name == "calloc" || name == "aligned_alloc" ||
           name == "operator new" || name == "operator new[]";
}

bool isMemorySafetyDeallocationFunction(const llvm::Function& function) {
    const std::string name = memorySafetyFunctionName(function);
    return name == "free" || name == "operator delete" ||
           name == "operator delete[]";
}

ReachableBlocks findReachableBlocks(const llvm::Function& function) {
    ReachableBlocks reachable;
    std::vector<const llvm::BasicBlock*> worklist{&function.getEntryBlock()};
    while (!worklist.empty()) {
        const llvm::BasicBlock* block = worklist.back();
        worklist.pop_back();
        if (!reachable.insert(block).second)
            continue;
        if (const auto* branch =
                llvm::dyn_cast<llvm::BranchInst>(block->getTerminator())) {
            if (branch->isConditional()) {
                if (const auto* condition = llvm::dyn_cast<llvm::ConstantInt>(
                        branch->getCondition())) {
                    worklist.push_back(
                        branch->getSuccessor(condition->isZero() ? 1 : 0));
                    continue;
                }
            }
        }
        for (const llvm::BasicBlock* successor : llvm::successors(block))
            worklist.push_back(successor);
    }
    return reachable;
}

bool isKnownNullPointer(const llvm::Value* pointer,
                        const PointerAliases& aliases,
                        std::unordered_set<const llvm::Value*>& visited,
                        unsigned depth = 0) {
    if (!pointer || depth > 16)
        return false;
    pointer = pointer->stripPointerCasts();
    if (isNullPointer(pointer))
        return true;
    if (!visited.insert(pointer).second)
        return false;
    const auto alias = aliases.find(pointer);
    return alias != aliases.end() &&
           isKnownNullPointer(alias->second, aliases, visited, depth + 1);
}

bool isKnownNullPointer(const llvm::Value* pointer,
                        const PointerAliases& aliases) {
    std::unordered_set<const llvm::Value*> visited;
    return isKnownNullPointer(pointer, aliases, visited);
}

bool getRemainingObjectSize(const llvm::Value* pointer,
                            const llvm::DataLayout& dataLayout,
                            const PointerAliases& aliases,
                            uint64_t& remainingSize, int64_t offset = 0,
                            unsigned depth = 0);

void reportOutOfBoundsAccess(const std::string& input,
                             const llvm::Module& module,
                             const llvm::Function& function,
                             const llvm::Instruction& instruction,
                             const llvm::Value* pointer,
                             const PointerAliases& aliases,
                             std::vector<Finding>& findings) {
    const llvm::Value* accessPointer = pointer->stripPointerCasts();
    if (isKnownNullPointer(accessPointer, aliases))
        return;

    uint64_t accessSize = 0;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
        accessSize = module.getDataLayout()
                         .getTypeStoreSize(load->getType())
                         .getFixedValue();
    else if (const auto* store =
                 llvm::dyn_cast<llvm::StoreInst>(&instruction))
        accessSize = module.getDataLayout()
                         .getTypeStoreSize(store->getValueOperand()->getType())
                         .getFixedValue();
    else if (const auto* atomic =
                 llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction))
        accessSize = module.getDataLayout()
                         .getTypeStoreSize(atomic->getValOperand()->getType())
                         .getFixedValue();
    else if (const auto* atomic =
                 llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction))
        accessSize =
            module.getDataLayout()
                .getTypeStoreSize(atomic->getCompareOperand()->getType())
                .getFixedValue();

    const bool isWrite =
        llvm::isa<llvm::StoreInst>(instruction) ||
        llvm::isa<llvm::AtomicRMWInst>(instruction) ||
        llvm::isa<llvm::AtomicCmpXchgInst>(instruction);

    pointer = accessPointer;
    while (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
        if (hasOutOfBoundsIndex(*gep)) {
            uint64_t remainingSize = 0;
            const bool withinFlexibleAllocation =
                hasOnlyFlexibleArrayOutOfBoundsIndex(*gep) && accessSize != 0 &&
                getRemainingObjectSize(accessPointer, module.getDataLayout(),
                                       aliases, remainingSize) &&
                accessSize <= remainingSize;
            if (withinFlexibleAllocation) {
                pointer = gep->getPointerOperand()->stripPointerCasts();
                continue;
            }
            const char* cwe = isWrite ? "CWE-787" : "CWE-125";
            addFinding(findings, input, function, instruction,
                       "OUT_OF_BOUNDS_ACCESS", cwe, "high",
                       "memory-safety",
                       "A constant array, vector, or structure index is "
                       "outside the indexed type's bounds.");
            return;
        }
        pointer = gep->getPointerOperand()->stripPointerCasts();
    }

    uint64_t remainingSize = 0;
    if (accessSize != 0 && !isNullPointer(accessPointer) &&
        getRemainingObjectSize(accessPointer, module.getDataLayout(), aliases,
                               remainingSize) &&
        accessSize > remainingSize) {
        const char* cwe = isWrite ? "CWE-787" : "CWE-125";
        addFinding(findings, input, function, instruction,
                   "OUT_OF_BOUNDS_ACCESS", cwe, "high", "memory-safety",
                   "Memory access extends beyond the remaining known object "
                   "size.");
    }
}

using PointerOrigins =
    std::unordered_map<const llvm::Value*, const llvm::Value*>;
using FreedPointers = std::unordered_set<const llvm::Value*>;

PointerAliases buildPointerAliases(const llvm::Function& function,
                                   const ReachableBlocks& reachable) {
    std::map<const llvm::AllocaInst*, std::set<const llvm::Value*>> slotValues;
    for (const llvm::BasicBlock& block : function) {
        if (reachable.count(&block) == 0)
            continue;
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (!store ||
                !store->getValueOperand()->getType()->isPointerTy())
                continue;
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                store->getPointerOperand()->stripPointerCasts());
            if (slot)
                slotValues[slot].insert(store->getValueOperand());
        }
    }

    PointerAliases aliases;
    for (const llvm::BasicBlock& block : function) {
        if (reachable.count(&block) == 0)
            continue;
        for (const llvm::Instruction& instruction : block) {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
            if (!load || !load->getType()->isPointerTy())
                continue;
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                load->getPointerOperand()->stripPointerCasts());
            if (!slot)
                continue;
            const auto found = slotValues.find(slot);
            if (found != slotValues.end() && found->second.size() == 1)
                aliases.emplace(load, *found->second.begin());
        }
    }
    return aliases;
}

bool getRemainingObjectSize(const llvm::Value* pointer,
                            const llvm::DataLayout& dataLayout,
                            const PointerAliases& aliases,
                            uint64_t& remainingSize, int64_t offset,
                            unsigned depth) {
    if (!pointer || depth > 16)
        return false;
    pointer = pointer->stripPointerCasts();
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pointer)) {
        llvm::APInt byteOffset(
            dataLayout.getIndexTypeSizeInBits(gep->getPointerOperandType()), 0);
        if (!gep->accumulateConstantOffset(dataLayout, byteOffset) ||
            !byteOffset.isSignedIntN(64))
            return false;
        const int64_t delta = byteOffset.getSExtValue();
        if ((delta > 0 &&
             offset > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 &&
             offset < std::numeric_limits<int64_t>::min() - delta))
            return false;
        return getRemainingObjectSize(gep->getPointerOperand(), dataLayout,
                                      aliases, remainingSize, offset + delta,
                                      depth + 1);
    }
    const auto alias = aliases.find(pointer);
    if (alias != aliases.end())
        return getRemainingObjectSize(alias->second, dataLayout, aliases,
                                      remainingSize, offset, depth + 1);

    uint64_t objectSize = 0;
    if (!llvm::getObjectSize(pointer, objectSize, dataLayout, nullptr))
        return false;
    if (offset < 0 || static_cast<uint64_t>(offset) > objectSize) {
        remainingSize = 0;
        return true;
    }
    remainingSize = objectSize - static_cast<uint64_t>(offset);
    return true;
}

const llvm::Value* pointerOrigin(const llvm::Value* value,
                                 const PointerOrigins& origins) {
    value = value->stripPointerCasts();
    const auto found = origins.find(value);
    if (found != origins.end())
        return found->second;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
        return pointerOrigin(gep->getPointerOperand(), origins);
    return value;
}

bool mayReferToFreedPointer(
    const llvm::Value* value, const PointerOrigins& origins,
    const FreedPointers& freed,
    std::unordered_set<const llvm::Value*>& visited, unsigned depth = 0) {
    if (!value || depth > 16)
        return false;
    value = value->stripPointerCasts();
    if (!visited.insert(value).second)
        return false;

    const auto found = origins.find(value);
    if (found != origins.end() && found->second != value)
        return mayReferToFreedPointer(found->second, origins, freed, visited,
                                      depth + 1);
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
        return mayReferToFreedPointer(gep->getPointerOperand(), origins,
                                      freed, visited, depth + 1);
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        for (const llvm::Value* incoming : phi->incoming_values()) {
            if (mayReferToFreedPointer(incoming, origins, freed, visited,
                                       depth + 1))
                return true;
        }
    } else if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        return mayReferToFreedPointer(select->getTrueValue(), origins, freed,
                                      visited, depth + 1) ||
               mayReferToFreedPointer(select->getFalseValue(), origins, freed,
                                      visited, depth + 1);
    }
    return freed.count(value) != 0;
}

bool mayReferToFreedPointer(const llvm::Value* value,
                            const PointerOrigins& origins,
                            const FreedPointers& freed) {
    std::unordered_set<const llvm::Value*> visited;
    return mayReferToFreedPointer(value, origins, freed, visited);
}

bool isZeroOffsetPointer(const llvm::Value* value) {
    value = value->stripPointerCasts();
    const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value);
    if (!gep)
        return true;
    for (const llvm::Use& indexUse : gep->indices()) {
        const auto* index = llvm::dyn_cast<llvm::ConstantInt>(indexUse.get());
        if (!index || !index->isZero())
            return false;
    }
    return isZeroOffsetPointer(gep->getPointerOperand());
}

struct NullPointerFacts {
    std::unordered_set<const llvm::Value*> values;
    std::unordered_set<const llvm::Value*> aliases;
};

NullPointerFacts nullPointersAtBlockEntry(const llvm::BasicBlock& block,
                                          const PointerAliases& aliases) {
    NullPointerFacts knownNull;
    bool firstPredecessor = true;
    for (const llvm::BasicBlock* predecessor : llvm::predecessors(&block)) {
        std::unordered_set<const llvm::Value*> edgeNull;
        std::unordered_set<const llvm::Value*> edgeNullAliases;
        const auto* branch =
            llvm::dyn_cast<llvm::BranchInst>(predecessor->getTerminator());
        if (branch && branch->isConditional() &&
            branch->getSuccessor(0) != branch->getSuccessor(1)) {
            const auto* comparison =
                llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
            if (comparison &&
                (comparison->getPredicate() == llvm::ICmpInst::ICMP_EQ ||
                 comparison->getPredicate() == llvm::ICmpInst::ICMP_NE)) {
                const llvm::Value* pointer = nullptr;
                if (isNullPointer(comparison->getOperand(0)))
                    pointer = comparison->getOperand(1);
                else if (isNullPointer(comparison->getOperand(1)))
                    pointer = comparison->getOperand(0);

                if (pointer && pointer->getType()->isPointerTy()) {
                    const bool takesTrueEdge =
                        branch->getSuccessor(0) == &block;
                    const bool isNullOnTrueEdge =
                        comparison->getPredicate() == llvm::ICmpInst::ICMP_EQ;
                    if (takesTrueEdge == isNullOnTrueEdge) {
                        pointer = pointer->stripPointerCasts();
                        edgeNull.insert(pointer);
                        const auto alias = aliases.find(pointer);
                        if (alias != aliases.end())
                            edgeNullAliases.insert(
                                alias->second->stripPointerCasts());
                    }
                }
            }
        }

        if (firstPredecessor) {
            knownNull.values = std::move(edgeNull);
            knownNull.aliases = std::move(edgeNullAliases);
            firstPredecessor = false;
        } else {
            for (auto it = knownNull.values.begin();
                 it != knownNull.values.end();) {
                if (edgeNull.count(*it) == 0)
                    it = knownNull.values.erase(it);
                else
                    ++it;
            }
            for (auto it = knownNull.aliases.begin();
                 it != knownNull.aliases.end();) {
                if (edgeNullAliases.count(*it) == 0)
                    it = knownNull.aliases.erase(it);
                else
                    ++it;
            }
        }
    }
    return knownNull;
}

} // namespace

void analyzeMemorySafety(const llvm::Module& module, const std::string& input,
                         std::vector<Finding>& findings) {
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;

        const ReachableBlocks reachable = findReachableBlocks(function);
        PointerOrigins argumentOrigins;
        const PointerAliases pointerAliases =
            buildPointerAliases(function, reachable);
        for (const llvm::Argument& argument : function.args()) {
            if (argument.getType()->isPointerTy())
                argumentOrigins.emplace(&argument, &argument);
        }

        PointerOrigins functionPointerOrigins = argumentOrigins;
        for (const llvm::BasicBlock& block : function) {
            if (reachable.count(&block) == 0)
                continue;
            for (const llvm::Instruction& instruction : block) {
                if (const auto* gep =
                        llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
                    functionPointerOrigins[gep] =
                        pointerOrigin(gep->getPointerOperand(),
                                      functionPointerOrigins);
                } else if (const auto* cast =
                               llvm::dyn_cast<llvm::CastInst>(&instruction)) {
                    if (cast->getType()->isPointerTy() &&
                        cast->getOperand(0)->getType()->isPointerTy())
                        functionPointerOrigins[cast] =
                            pointerOrigin(cast->getOperand(0),
                                          functionPointerOrigins);
                } else if (const auto* call =
                               llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    const auto* callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    if (callee && call->getType()->isPointerTy() &&
                        isMemorySafetyAllocationFunction(*callee))
                        functionPointerOrigins[call] = call;
                }
            }
        }
        for (const auto& alias : pointerAliases)
            functionPointerOrigins[alias.first] =
                pointerOrigin(alias.second, functionPointerOrigins);

        std::unordered_map<const llvm::BasicBlock*, FreedPointers> freedIn;
        std::unordered_map<const llvm::BasicBlock*, FreedPointers> freedOut;
        bool freedStateChanged;
        do {
            freedStateChanged = false;
            for (const llvm::BasicBlock& block : function) {
                if (reachable.count(&block) == 0)
                    continue;
                FreedPointers incoming;
                if (&block != &function.getEntryBlock()) {
                    for (const llvm::BasicBlock* predecessor :
                         llvm::predecessors(&block)) {
                        if (reachable.count(predecessor) == 0)
                            continue;
                        incoming.insert(freedOut[predecessor].begin(),
                                        freedOut[predecessor].end());
                    }
                }

                FreedPointers outgoing = incoming;
                for (const llvm::Instruction& instruction : block) {
                    const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call)
                        continue;
                    const auto* callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    if (!callee)
                        continue;
                    const std::string name = memorySafetyFunctionName(*callee);
                    if (isMemorySafetyAllocationFunction(*callee) &&
                        call->getType()->isPointerTy()) {
                        outgoing.erase(call);
                    } else if (isMemorySafetyDeallocationFunction(*callee) &&
                               call->arg_size() > 0 &&
                               isZeroOffsetPointer(call->getArgOperand(0))) {
                        const llvm::Value* origin = pointerOrigin(
                            call->getArgOperand(0), functionPointerOrigins);
                        if (!isNullPointer(origin))
                            outgoing.insert(origin);
                    }
                }
                if (freedIn[&block] != incoming ||
                    freedOut[&block] != outgoing) {
                    freedIn[&block] = std::move(incoming);
                    freedOut[&block] = std::move(outgoing);
                    freedStateChanged = true;
                }
            }
        } while (freedStateChanged);

        for (const llvm::BasicBlock& block : function) {
            if (reachable.count(&block) == 0)
                continue;
            std::unordered_set<const llvm::Value*> nullPointerSlots;
            NullPointerFacts nullPointerFacts =
                nullPointersAtBlockEntry(block, pointerAliases);
            std::unordered_set<const llvm::Value*> nullPointerValues =
                std::move(nullPointerFacts.values);
            std::unordered_set<const llvm::Value*> nullPointerAliases =
                std::move(nullPointerFacts.aliases);
            PointerOrigins pointerOrigins = functionPointerOrigins;
            PointerOrigins pointerSlots;
            FreedPointers freedPointers = freedIn[&block];
            for (const llvm::Instruction& instruction : block) {
                if (const auto* load =
                        llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    const llvm::Value* pointer =
                        load->getPointerOperand()->stripPointerCasts();
                    const auto alias = pointerAliases.find(pointer);
                    if (isNullPointer(pointer) ||
                        nullPointerValues.count(pointer) != 0 ||
                        (alias != pointerAliases.end() &&
                         (nullPointerValues.count(
                              alias->second->stripPointerCasts()) != 0 ||
                          nullPointerAliases.count(
                              alias->second->stripPointerCasts()) != 0))) {
                        addFinding(findings, input, function, instruction,
                                   "NULL_DEREFERENCE", "CWE-476", "high",
                                   "memory-safety",
                                   "Load instruction dereferences a null "
                                   "pointer.");
                    }
                    if (mayReferToFreedPointer(load->getPointerOperand(),
                                               pointerOrigins,
                                               freedPointers)) {
                        addFinding(findings, input, function, instruction,
                                   "USE_AFTER_FREE", "CWE-416", "high",
                                   "memory-safety",
                                   "Load instruction reads memory after its "
                                   "allocation was released.");
                    }
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(pointer);
                    if (slot && load->getType()->isPointerTy() &&
                        nullPointerSlots.count(slot) != 0)
                        nullPointerValues.insert(load);
                    if (slot && load->getType()->isPointerTy()) {
                        const auto storedOrigin = pointerSlots.find(slot);
                        if (storedOrigin != pointerSlots.end())
                            pointerOrigins[load] = storedOrigin->second;
                    }
                    reportOutOfBoundsAccess(input, module, function, instruction,
                                            load->getPointerOperand(),
                                            pointerAliases,
                                            findings);
                } else if (const auto* store =
                               llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (isNullPointer(store->getPointerOperand())) {
                        addFinding(findings, input, function, instruction,
                                   "NULL_DEREFERENCE", "CWE-476", "high",
                                   "memory-safety",
                                   "Store instruction writes through a null "
                                   "pointer.");
                    }
                    const llvm::Value* destination =
                        store->getPointerOperand()->stripPointerCasts();
                    if (mayReferToFreedPointer(store->getPointerOperand(),
                                               pointerOrigins,
                                               freedPointers)) {
                        addFinding(findings, input, function, instruction,
                                   "USE_AFTER_FREE", "CWE-416", "high",
                                   "memory-safety",
                                   "Store instruction writes to memory after "
                                   "its allocation was released.");
                    }
                    const llvm::Value* slot =
                        destination;
                    if (llvm::isa<llvm::AllocaInst>(slot) &&
                        store->getValueOperand()->getType()->isPointerTy()) {
                        nullPointerSlots.erase(slot);
                        const llvm::Value* storedValue =
                            store->getValueOperand()->stripPointerCasts();
                        if (isNullPointer(storedValue) ||
                            nullPointerValues.count(storedValue) != 0)
                            nullPointerSlots.insert(slot);
                        pointerSlots[slot] =
                            pointerOrigin(store->getValueOperand(),
                                          pointerOrigins);
                    }
                    reportOutOfBoundsAccess(input, module, function, instruction,
                                            store->getPointerOperand(),
                                            pointerAliases,
                                            findings);
                } else if (const auto* atomic =
                               llvm::dyn_cast<llvm::AtomicRMWInst>(
                                   &instruction)) {
                    if (isNullPointer(atomic->getPointerOperand())) {
                        addFinding(findings, input, function, instruction,
                                   "NULL_DEREFERENCE", "CWE-476", "high",
                                   "memory-safety",
                                   "Atomic operation uses a null pointer.");
                    }
                    if (mayReferToFreedPointer(atomic->getPointerOperand(),
                                               pointerOrigins,
                                               freedPointers))
                        addFinding(findings, input, function, instruction,
                                   "USE_AFTER_FREE", "CWE-416", "high",
                                   "memory-safety",
                                   "Atomic operation accesses memory after "
                                   "its allocation was released.");
                    reportOutOfBoundsAccess(input, module, function,
                                            instruction,
                                            atomic->getPointerOperand(),
                                            pointerAliases, findings);
                } else if (const auto* atomic =
                               llvm::dyn_cast<llvm::AtomicCmpXchgInst>(
                                   &instruction)) {
                    if (isNullPointer(atomic->getPointerOperand())) {
                        addFinding(findings, input, function, instruction,
                                   "NULL_DEREFERENCE", "CWE-476", "high",
                                   "memory-safety",
                                   "Atomic operation uses a null pointer.");
                    }
                    if (mayReferToFreedPointer(atomic->getPointerOperand(),
                                               pointerOrigins,
                                               freedPointers))
                        addFinding(findings, input, function, instruction,
                                   "USE_AFTER_FREE", "CWE-416", "high",
                                   "memory-safety",
                                   "Atomic operation accesses memory after "
                                   "its allocation was released.");
                    reportOutOfBoundsAccess(input, module, function,
                                            instruction,
                                            atomic->getPointerOperand(),
                                            pointerAliases, findings);
                }

                if (const auto* binary =
                        llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
                    const unsigned opcode = binary->getOpcode();
                    const bool isDivision =
                        opcode == llvm::Instruction::SDiv ||
                        opcode == llvm::Instruction::UDiv ||
                        opcode == llvm::Instruction::SRem ||
                        opcode == llvm::Instruction::URem;
                    const auto* divisor =
                        llvm::dyn_cast<llvm::ConstantInt>(
                            binary->getOperand(1));
                    if (isDivision && divisor && divisor->isZero()) {
                        addFinding(findings, input, function, instruction,
                                   "DIVISION_BY_ZERO", "CWE-369", "high",
                                   "undefined-behavior",
                                   "Integer division or remainder has a "
                                   "constant zero divisor.");
                    }
                }

                if (const auto* gep =
                        llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
                    pointerOrigins[gep] =
                        pointerOrigin(gep->getPointerOperand(),
                                      pointerOrigins);
                }

                if (const auto* cast =
                        llvm::dyn_cast<llvm::CastInst>(&instruction)) {
                    if (cast->getType()->isPointerTy() &&
                        cast->getOperand(0)->getType()->isPointerTy())
                        pointerOrigins[cast] =
                            pointerOrigin(cast->getOperand(0), pointerOrigins);
                }

                if (const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    const auto* callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    const std::string name =
                        callee ? memorySafetyFunctionName(*callee)
                               : std::string();
                    if (callee && isMemorySafetyDeallocationFunction(*callee) &&
                        call->arg_size() > 0 &&
                        isZeroOffsetPointer(call->getArgOperand(0))) {
                        const llvm::Value* origin =
                            pointerOrigin(call->getArgOperand(0),
                                          pointerOrigins);
                        if (!isNullPointer(origin)) {
                            if (freedPointers.count(origin) != 0) {
                                addFinding(findings, input, function,
                                           instruction, "DOUBLE_FREE",
                                           "CWE-415", "high",
                                           "memory-safety",
                                           name + " releases the same pointer "
                                                  "more than once.");
                            } else {
                                freedPointers.insert(origin);
                            }
                        }
                    } else if (callee &&
                               isMemorySafetyAllocationFunction(*callee) &&
                               call->getType()->isPointerTy()) {
                        pointerOrigins[call] = call;
                    }
                    nullPointerSlots.clear();
                    nullPointerAliases.clear();
                }

                if (const auto* returned =
                        llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
                    const llvm::Value* returnValue = returned->getReturnValue();
                    if (returnValue && returnValue->getType()->isPointerTy()) {
                        const llvm::Value* base =
                            llvm::getUnderlyingObject(returnValue);
                        if (llvm::isa<llvm::AllocaInst>(base)) {
                            addFinding(findings, input, function, instruction,
                                       "RETURN_STACK_ADDRESS", "CWE-562",
                                       "high", "memory-safety",
                                       "Function returns a pointer to a local "
                                       "stack allocation.");
                        }
                        if (mayReferToFreedPointer(returnValue,
                                                   pointerOrigins,
                                                   freedPointers))
                            addFinding(findings, input, function, instruction,
                                       "USE_AFTER_FREE", "CWE-416", "high",
                                       "memory-safety",
                                       "Function returns a pointer after its "
                                       "allocation was released.");
                    }
                }
            }
        }
    }
}

} // namespace buganalyzer
