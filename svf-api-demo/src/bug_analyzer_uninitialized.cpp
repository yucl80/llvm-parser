#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace buganalyzer {
namespace {

struct MemoryLocation {
    const llvm::Value* object;
    int64_t offset;
    bool unknownOffset = false;

    friend bool operator==(const MemoryLocation& left,
                           const MemoryLocation& right) {
        return left.object == right.object && left.offset == right.offset &&
               left.unknownOffset == right.unknownOffset;
    }

    friend bool operator<(const MemoryLocation& left,
                          const MemoryLocation& right) {
        if (left.object != right.object)
            return std::less<const llvm::Value*>()(left.object, right.object);
        if (left.offset != right.offset)
            return left.offset < right.offset;
        return left.unknownOffset < right.unknownOffset;
    }

};

struct MemoryObject {
    uint64_t size;
    bool zeroInitialized;
};

using MemoryObjects =
    std::unordered_map<const llvm::Value*, MemoryObject>;
using PointerReferences =
    std::unordered_map<const llvm::Value*, std::vector<MemoryLocation>>;
using EscapedObjects = std::set<const llvm::Value*>;
using ReachableBlocks = std::set<const llvm::BasicBlock*>;

ReachableBlocks reachableBlocks(const llvm::Function& function) {
    ReachableBlocks reachable;
    std::vector<const llvm::BasicBlock*> worklist{&function.getEntryBlock()};
    while (!worklist.empty()) {
        const llvm::BasicBlock* block = worklist.back();
        worklist.pop_back();
        if (!reachable.insert(block).second)
            continue;
        for (const llvm::BasicBlock* successor : llvm::successors(block))
            worklist.push_back(successor);
    }
    return reachable;
}

std::string valueLabel(const llvm::Value& value) {
    if (value.hasName())
        return value.getName().str();
    std::string result;
    llvm::raw_string_ostream stream(result);
    value.printAsOperand(stream, false);
    stream.flush();
    return result;
}

uint64_t fixedTypeSize(llvm::Type* type,
                       const llvm::DataLayout& dataLayout) {
    const llvm::TypeSize size = dataLayout.getTypeAllocSize(type);
    return size.isScalable() ? 0 : size.getFixedValue();
}

std::optional<uint64_t> constantUnsigned(const llvm::Value* value) {
    if (!value)
        return std::nullopt;
    const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value);
    if (!constant || constant->getValue().isNegative())
        return std::nullopt;
    return constant->getValue().getLimitedValue();
}

bool allocationCall(const llvm::CallBase& call, std::string& name) {
    const auto* callee = llvm::dyn_cast<llvm::Function>(
        call.getCalledOperand()->stripPointerCasts());
    if (!callee)
        return false;
    name = simpleFunctionName(*callee);
    return isAllocationFunction(name) || name == "calloc";
}

MemoryObjects collectMemoryObjects(const llvm::Function& function,
                                   const llvm::DataLayout& dataLayout) {
    MemoryObjects objects;
    for (const llvm::BasicBlock& block : function) {
        for (const llvm::Instruction& instruction : block) {
            if (const auto* allocation =
                    llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                uint64_t size = fixedTypeSize(allocation->getAllocatedType(),
                                              dataLayout);
                const auto count = constantUnsigned(allocation->getArraySize());
                if (!count || (size != 0 &&
                               *count >
                                   std::numeric_limits<uint64_t>::max() / size))
                    continue;
                size *= *count;
                if (size != 0)
                    objects.emplace(allocation, MemoryObject{size, false});
                continue;
            }

            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call)
                continue;
            std::string name;
            if (!allocationCall(*call, name))
                continue;

            std::optional<uint64_t> size;
            bool zeroInitialized = name == "calloc";
            if (name == "calloc" && call->arg_size() >= 2) {
                const auto count = constantUnsigned(call->getArgOperand(0));
                const auto elementSize =
                    constantUnsigned(call->getArgOperand(1));
                if (count && elementSize &&
                    (*count == 0 ||
                     *elementSize <=
                         std::numeric_limits<uint64_t>::max() / *count))
                    size = *count * *elementSize;
            } else if (name == "aligned_alloc" && call->arg_size() >= 2) {
                size = constantUnsigned(call->getArgOperand(1));
            } else if (call->arg_size() > 0) {
                size = constantUnsigned(call->getArgOperand(0));
            }
            if (size && *size != 0)
                objects.emplace(call, MemoryObject{*size, zeroInitialized});
        }
    }
    return objects;
}

std::vector<MemoryLocation> resolveMemoryLocations(
    const llvm::Value* pointer, const llvm::DataLayout& dataLayout,
    const MemoryObjects& objects, const PointerReferences& references,
    unsigned depth = 0) {
    if (!pointer || depth > 16)
        return {};
    pointer = pointer->stripPointerCasts();
    const auto reference = references.find(pointer);
    if (reference != references.end())
        return reference->second;
    if (objects.count(pointer) != 0)
        return {{pointer, 0}};

    std::vector<MemoryLocation> locations;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pointer)) {
        llvm::APInt byteOffset(
            dataLayout.getIndexTypeSizeInBits(gep->getPointerOperandType()), 0);
        locations = resolveMemoryLocations(gep->getPointerOperand(),
                                           dataLayout, objects, references,
                                           depth + 1);
        if (!gep->accumulateConstantOffset(dataLayout, byteOffset) ||
            !byteOffset.isSignedIntN(64)) {
            for (MemoryLocation& location : locations)
                location.unknownOffset = true;
        } else {
            const int64_t offset = byteOffset.getSExtValue();
            for (MemoryLocation& location : locations) {
                if (location.unknownOffset)
                    continue;
                if ((offset > 0 &&
                     location.offset >
                         std::numeric_limits<int64_t>::max() - offset) ||
                    (offset < 0 &&
                     location.offset <
                         std::numeric_limits<int64_t>::min() - offset))
                    return {};
                location.offset += offset;
            }
        }
    } else if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(pointer)) {
        for (const llvm::Value* choice :
             {select->getTrueValue(), select->getFalseValue()}) {
            auto choices = resolveMemoryLocations(choice, dataLayout, objects,
                                                  references, depth + 1);
            locations.insert(locations.end(), choices.begin(), choices.end());
        }
    } else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(pointer)) {
        for (const llvm::Value* incoming : phi->incoming_values()) {
            auto choices = resolveMemoryLocations(incoming, dataLayout,
                                                  objects, references,
                                                  depth + 1);
            locations.insert(locations.end(), choices.begin(), choices.end());
        }
    }

    std::sort(locations.begin(), locations.end());
    locations.erase(std::unique(locations.begin(), locations.end()),
                    locations.end());
    return locations;
}

bool hasUnknownPointerTarget(const llvm::Value* pointer,
                             const MemoryObjects& objects,
                             const PointerReferences& references,
                             unsigned depth = 0) {
    if (!pointer || depth > 16)
        return true;
    pointer = pointer->stripPointerCasts();
    if (references.count(pointer) != 0 || objects.count(pointer) != 0)
        return false;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pointer))
        return hasUnknownPointerTarget(gep->getPointerOperand(), objects,
                                       references, depth + 1);
    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(pointer))
        return hasUnknownPointerTarget(select->getTrueValue(), objects,
                                       references, depth + 1) ||
               hasUnknownPointerTarget(select->getFalseValue(), objects,
                                       references, depth + 1);
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(pointer)) {
        for (const llvm::Value* incoming : phi->incoming_values()) {
            if (hasUnknownPointerTarget(incoming, objects, references,
                                        depth + 1))
                return true;
        }
        return false;
    }
    return true;
}

std::optional<MemoryLocation> resolveMemoryLocation(
    const llvm::Value* pointer, const llvm::DataLayout& dataLayout,
    const MemoryObjects& objects, const PointerReferences& references,
    unsigned depth = 0) {
    std::vector<MemoryLocation> locations = resolveMemoryLocations(
        pointer, dataLayout, objects, references, depth);
    if (locations.size() == 1 &&
        !hasUnknownPointerTarget(pointer, objects, references))
        return locations.front();
    return std::nullopt;
}

PointerReferences buildPointerReferences(
    const llvm::Function& function, const llvm::DataLayout& dataLayout,
    const MemoryObjects& objects) {
    PointerReferences references;
    struct SlotValue {
        std::set<MemoryLocation> locations;
        bool unknown = false;

        bool operator==(const SlotValue& other) const {
            return locations == other.locations && unknown == other.unknown;
        }
    };
    using SlotState = std::map<const llvm::AllocaInst*, SlotValue>;
    // A loop that advances a pointer slot (e.g. a range-based for) can make
    // the same object appear with a different offset on every iteration.
    // Widen such merges to an unknown offset, otherwise the fixpoint never
    // converges because each iteration adds one more offset to the set.
    auto widenMergedState = [](SlotState& state) {
        for (auto& entry : state) {
            std::set<MemoryLocation> widened;
            for (auto first = entry.second.locations.begin();
                 first != entry.second.locations.end();) {
                auto last = first;
                while (last != entry.second.locations.end() &&
                       last->object == first->object)
                    ++last;
                if (std::next(first) == last)
                    widened.insert(*first);
                else
                    widened.insert(MemoryLocation{first->object, 0, true});
                first = last;
            }
            entry.second.locations = std::move(widened);
        }
    };
    std::vector<const llvm::AllocaInst*> slots;
    for (const llvm::BasicBlock& block : function) {
        for (const llvm::Instruction& instruction : block) {
            const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
            if (alloca && alloca->getAllocatedType()->isPointerTy())
                slots.push_back(alloca);
        }
    }

    const ReachableBlocks reachable = reachableBlocks(function);
    SlotState entryState;
    for (const llvm::AllocaInst* slot : slots)
        entryState[slot].unknown = true;

    bool referencesChanged;
    do {
        // Only compare the settled map: in/out are rebuilt on every pass, so
        // transient reference changes inside a pass must not keep the outer
        // fixpoint running.
        const PointerReferences previousReferences = references;
        referencesChanged = false;
        std::unordered_map<const llvm::BasicBlock*, SlotState> in, out;
        bool changed;
        do {
            changed = false;
            for (const llvm::BasicBlock& block : function) {
                if (reachable.count(&block) == 0)
                    continue;

                SlotState nextIn;
                if (&block == &function.getEntryBlock()) {
                    nextIn = entryState;
                } else {
                    for (const llvm::BasicBlock* predecessor :
                         llvm::predecessors(&block)) {
                        if (reachable.count(predecessor) == 0)
                            continue;
                        for (const auto& entry : out[predecessor]) {
                            SlotValue& value = nextIn[entry.first];
                            value.locations.insert(entry.second.locations.begin(),
                                                   entry.second.locations.end());
                            value.unknown =
                                value.unknown || entry.second.unknown;
                        }
                    }
                    widenMergedState(nextIn);
                }

                SlotState nextOut = nextIn;
                for (const llvm::Instruction& instruction : block) {
                    if (const auto* store =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (!store->getValueOperand()->getType()->isPointerTy())
                            continue;
                        const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                            store->getPointerOperand()->stripPointerCasts());
                        if (!slot || nextOut.count(slot) == 0)
                            continue;

                        SlotValue value;
                        const auto locations = resolveMemoryLocations(
                            store->getValueOperand(), dataLayout, objects,
                            references);
                        value.locations.insert(locations.begin(),
                                               locations.end());
                        value.unknown =
                            locations.empty() ||
                            hasUnknownPointerTarget(store->getValueOperand(),
                                                    objects, references);
                        nextOut[slot] = std::move(value);
                    } else if (const auto* load =
                                   llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                        if (!load->getType()->isPointerTy())
                            continue;
                        const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                            load->getPointerOperand()->stripPointerCasts());
                        if (!slot)
                            continue;

                        const auto value = nextOut.find(slot);
                        if (value == nextOut.end() || value->second.unknown ||
                            value->second.locations.empty()) {
                            references.erase(load);
                            continue;
                        }
                        std::vector<MemoryLocation> locations(
                            value->second.locations.begin(),
                            value->second.locations.end());
                        const auto current = references.find(load);
                        if (current == references.end() ||
                            current->second != locations)
                            references[load] = std::move(locations);
                    }
                }

                if (in[&block] != nextIn || out[&block] != nextOut) {
                    in[&block] = std::move(nextIn);
                    out[&block] = std::move(nextOut);
                    changed = true;
                }
            }
        } while (changed);
        referencesChanged = references != previousReferences;
    } while (referencesChanged);
    return references;
}

struct ByteRange {
    uint64_t begin;
    uint64_t end;

    friend bool operator==(const ByteRange& left, const ByteRange& right) {
        return left.begin == right.begin && left.end == right.end;
    }
};

using InitializedRanges = std::vector<ByteRange>;
using InitializationState =
    std::unordered_map<const llvm::Value*, InitializedRanges>;

void addInitializedRange(InitializedRanges& ranges, uint64_t begin,
                         uint64_t end) {
    if (begin >= end)
        return;
    InitializedRanges result;
    bool inserted = false;
    for (const ByteRange& range : ranges) {
        if (range.end < begin) {
            result.push_back(range);
        } else if (end < range.begin) {
            if (!inserted) {
                result.push_back({begin, end});
                inserted = true;
            }
            result.push_back(range);
        } else {
            begin = std::min(begin, range.begin);
            end = std::max(end, range.end);
        }
    }
    if (!inserted)
        result.push_back({begin, end});
    ranges.swap(result);
}

InitializedRanges intersectRanges(const InitializedRanges& left,
                                  const InitializedRanges& right) {
    InitializedRanges result;
    std::size_t i = 0, j = 0;
    while (i < left.size() && j < right.size()) {
        const uint64_t begin = std::max(left[i].begin, right[j].begin);
        const uint64_t end = std::min(left[i].end, right[j].end);
        if (begin < end)
            result.push_back({begin, end});
        if (left[i].end < right[j].end)
            ++i;
        else
            ++j;
    }
    return result;
}

InitializationState intersectStates(const InitializationState& left,
                                    const InitializationState& right) {
    InitializationState result;
    for (const auto& entry : left) {
        const auto found = right.find(entry.first);
        if (found == right.end())
            continue;
        InitializedRanges common =
            intersectRanges(entry.second, found->second);
        if (!common.empty())
            result.emplace(entry.first, std::move(common));
    }
    return result;
}

InitializationState fullyInitializedState(const MemoryObjects& objects) {
    InitializationState state;
    for (const auto& object : objects) {
        if (object.second.size != 0)
            state.emplace(object.first,
                          InitializedRanges{{0, object.second.size}});
    }
    return state;
}

bool rangeIsInitialized(const InitializationState& state,
                        const llvm::Value* object, uint64_t begin,
                        uint64_t end) {
    if (begin >= end)
        return true;
    const auto found = state.find(object);
    if (found == state.end())
        return false;
    for (const ByteRange& range : found->second) {
        if (range.begin <= begin && range.end >= end)
            return true;
        if (range.begin > begin)
            return false;
    }
    return false;
}

bool findUninitializedByte(const InitializationState& state,
                           const llvm::Value* object, uint64_t objectSize,
                           uint64_t start, uint64_t& offset) {
    uint64_t initializedUntil = start;
    const auto found = state.find(object);
    if (found != state.end()) {
        for (const ByteRange& range : found->second) {
            if (range.end <= initializedUntil)
                continue;
            if (range.begin > initializedUntil) {
                offset = initializedUntil;
                return true;
            }
            initializedUntil = std::max(initializedUntil, range.end);
            if (initializedUntil >= objectSize)
                return false;
        }
    }
    if (initializedUntil < objectSize) {
        offset = initializedUntil;
        return true;
    }
    return false;
}

uint64_t typeStoreSize(llvm::Type* type,
                       const llvm::DataLayout& dataLayout) {
    const llvm::TypeSize size = dataLayout.getTypeStoreSize(type);
    return size.isScalable() ? 0 : size.getFixedValue();
}

bool locationRange(const MemoryLocation& location, uint64_t size,
                   const MemoryObjects& objects, uint64_t& begin,
                   uint64_t& end) {
    const auto found = objects.find(location.object);
    if (found == objects.end() || location.offset < 0 || size == 0)
        return false;
    begin = static_cast<uint64_t>(location.offset);
    if (size > std::numeric_limits<uint64_t>::max() - begin)
        return false;
    end = begin + size;
    return end <= found->second.size;
}

bool isInitialized(const InitializationState& state,
                   const MemoryLocation& location, uint64_t size,
                   const MemoryObjects& objects) {
    uint64_t begin = 0, end = 0;
    return locationRange(location, size, objects, begin, end) &&
           rangeIsInitialized(state, location.object, begin, end);
}

void markInitialized(InitializationState& state,
                     const MemoryLocation& location, uint64_t size,
                     const MemoryObjects& objects) {
    uint64_t begin = 0, end = 0;
    if (locationRange(location, size, objects, begin, end))
        addInitializedRange(state[location.object], begin, end);
}

void copyInitializedRanges(InitializationState& state,
                           const MemoryLocation& source,
                           const MemoryLocation& destination, uint64_t size,
                           const MemoryObjects& objects) {
    uint64_t sourceBegin = 0, sourceEnd = 0;
    uint64_t destinationBegin = 0, destinationEnd = 0;
    if (!locationRange(source, size, objects, sourceBegin, sourceEnd) ||
        !locationRange(destination, size, objects, destinationBegin,
                       destinationEnd) ||
        destinationEnd <= destinationBegin)
        return;

    const auto sourceRanges = state.find(source.object);
    if (sourceRanges == state.end())
        return;
    const InitializedRanges initializedSourceRanges = sourceRanges->second;
    for (const ByteRange& range : initializedSourceRanges) {
        const uint64_t begin = std::max(range.begin, sourceBegin);
        const uint64_t end = std::min(range.end, sourceEnd);
        if (begin < end)
            addInitializedRange(state[destination.object],
                                destinationBegin + (begin - sourceBegin),
                                destinationBegin + (end - sourceBegin));
    }
}

std::optional<uint64_t> memoryOperationLength(const llvm::Value* value) {
    return constantUnsigned(value);
}

bool isMemoryAllocatorCall(const llvm::Instruction& instruction,
                           std::string& name) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    return call && allocationCall(*call, name);
}

void applyMemoryWrites(const llvm::Instruction& instruction,
                       InitializationState& state,
                       const llvm::DataLayout& dataLayout,
                       const MemoryObjects& objects,
                       const PointerReferences& references) {
    if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        state.erase(alloca);
        return;
    }

    std::string allocationName;
    if (isMemoryAllocatorCall(instruction, allocationName)) {
        state.erase(&instruction);
        const auto object = objects.find(&instruction);
        if (object != objects.end() && object->second.zeroInitialized)
            addInitializedRange(state[&instruction], 0, object->second.size);
        return;
    }

    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        const auto location = resolveMemoryLocation(
            store->getPointerOperand(), dataLayout, objects, references);
        if (location)
            markInitialized(state, *location,
                            typeStoreSize(store->getValueOperand()->getType(),
                                          dataLayout),
                            objects);
        return;
    }

    const llvm::Value* destination = nullptr;
    const llvm::Value* lengthValue = nullptr;
    const llvm::Value* source = nullptr;
    if (const auto* set = llvm::dyn_cast<llvm::MemSetInst>(&instruction)) {
        destination = set->getDest();
        lengthValue = set->getLength();
    } else if (const auto* transfer =
                   llvm::dyn_cast<llvm::MemTransferInst>(&instruction)) {
        destination = transfer->getDest();
        source = transfer->getSource();
        lengthValue = transfer->getLength();
    } else if (const auto* call =
                   llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const auto* callee = llvm::dyn_cast<llvm::Function>(
            call->getCalledOperand()->stripPointerCasts());
        if (!callee)
            return;
        const std::string name = simpleFunctionName(*callee);
        if (name == "memset" && call->arg_size() >= 3) {
            destination = call->getArgOperand(0);
            lengthValue = call->getArgOperand(2);
        } else if ((name == "memcpy" || name == "memmove") &&
                   call->arg_size() >= 3) {
            destination = call->getArgOperand(0);
            source = call->getArgOperand(1);
            lengthValue = call->getArgOperand(2);
        }
    }
    const auto length = memoryOperationLength(lengthValue);
    if (!destination || !length || *length == 0)
        return;
    const auto destinationLocation = resolveMemoryLocation(
        destination, dataLayout, objects, references);
    if (!destinationLocation)
        return;
    if (source) {
        const auto sourceLocation = resolveMemoryLocation(
            source, dataLayout, objects, references);
        if (!sourceLocation)
            return;
        copyInitializedRanges(state, *sourceLocation, *destinationLocation,
                              *length, objects);
        return;
    }
    markInitialized(state, *destinationLocation, *length, objects);
}

InitializationState transferInitialization(
    const llvm::BasicBlock& block, InitializationState state,
    const llvm::DataLayout& dataLayout, const MemoryObjects& objects,
    const PointerReferences& references) {
    for (const llvm::Instruction& instruction : block)
        applyMemoryWrites(instruction, state, dataLayout, objects, references);
    return state;
}

EscapedObjects escapedByInstruction(
    const llvm::Instruction& instruction, const llvm::DataLayout& dataLayout,
    const MemoryObjects& objects, const PointerReferences& references) {
    EscapedObjects escaped;
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call || llvm::isa<llvm::MemIntrinsic>(call))
        return escaped;
    const auto* callee = llvm::dyn_cast<llvm::Function>(
        call->getCalledOperand()->stripPointerCasts());
    const std::string name =
        callee ? simpleFunctionName(*callee) : std::string();
    if (isAllocationFunction(name) || name == "calloc" ||
        isDeallocationFunction(name) || name == "memset" ||
        name == "memcpy" || name == "memmove")
        return escaped;
    for (const llvm::Use& argument : call->args()) {
        if (!argument->getType()->isPointerTy())
            continue;
        for (const MemoryLocation& location : resolveMemoryLocations(
                 argument.get(), dataLayout, objects, references)) {
            if (objects.count(location.object) != 0)
                escaped.insert(location.object);
        }
    }
    return escaped;
}

EscapedObjects transferEscapes(
    const llvm::BasicBlock& block, EscapedObjects escaped,
    const llvm::DataLayout& dataLayout, const MemoryObjects& objects,
    const PointerReferences& references) {
    for (const llvm::Instruction& instruction : block) {
        EscapedObjects instructionEscapes =
            escapedByInstruction(instruction, dataLayout, objects, references);
        escaped.insert(instructionEscapes.begin(), instructionEscapes.end());
    }
    return escaped;
}

void reportUninitializedRead(const llvm::Instruction& instruction,
                             const llvm::Value* pointer, uint64_t size,
                             const InitializationState& state,
                             const llvm::DataLayout& dataLayout,
                             const MemoryObjects& objects,
                             const PointerReferences& references,
                             const EscapedObjects& escaped,
                             const std::string& input,
                             const llvm::Function& function,
                             std::vector<Finding>& findings) {
    for (const MemoryLocation& location : resolveMemoryLocations(
             pointer, dataLayout, objects, references)) {
        if (escaped.count(location.object) != 0)
            continue;
        const auto object = objects.find(location.object);
        uint64_t begin = 0, end = 0;
        if (object == objects.end() || size == 0)
            continue;
        if (location.unknownOffset) {
            const uint64_t start =
                location.offset < 0
                    ? 0
                    : static_cast<uint64_t>(location.offset);
            if (start >= object->second.size ||
                size > object->second.size - start ||
                !findUninitializedByte(state, location.object,
                                       object->second.size, start, begin))
                continue;
            end = begin + 1;
        } else if (isInitialized(state, location, size, objects) ||
                   !locationRange(location, size, objects, begin, end)) {
            continue;
        }
        addFinding(findings, input, function, instruction, "UNINITIALIZED_READ",
                   "CWE-457", "medium", "memory-safety",
                   "Read of uninitialized bytes [" + std::to_string(begin) +
                       "," + std::to_string(end) + ") from object '" +
                       valueLabel(*location.object) + "'.");
    }
}

void analyzeUninitializedReads(const llvm::Function& function,
                               const llvm::DataLayout& dataLayout,
                               const std::string& input,
                               std::vector<Finding>& findings) {
    MemoryObjects objects = collectMemoryObjects(function, dataLayout);
    if (objects.empty())
        return;
    const PointerReferences references =
        buildPointerReferences(function, dataLayout, objects);

    // Must-analysis starts non-entry blocks at the lattice top. Starting at
    // the empty state incorrectly lets an uncomputed loop back-edge erase
    // initialization established before the loop.
    std::unordered_map<const llvm::BasicBlock*, InitializationState> in, out;
    std::unordered_map<const llvm::BasicBlock*, EscapedObjects> escapedIn,
        escapedOut;
    const llvm::BasicBlock* entry = &function.getEntryBlock();
    const ReachableBlocks reachable = reachableBlocks(function);
    const InitializationState top = fullyInitializedState(objects);
    for (const llvm::BasicBlock& block : function) {
        in[&block] = reachable.count(&block) != 0 ? top
                                                   : InitializationState{};
        out[&block] = reachable.count(&block) != 0 ? top
                                                    : InitializationState{};
        escapedIn[&block] = {};
        escapedOut[&block] = {};
    }
    in[entry] = {};

    bool changed;
    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            if (reachable.count(&block) == 0)
                continue;
            InitializationState nextIn;
            if (&block != entry) {
                bool firstPredecessor = true;
                for (const llvm::BasicBlock* predecessor :
                     llvm::predecessors(&block)) {
                    if (reachable.count(predecessor) == 0)
                        continue;
                    if (firstPredecessor) {
                        nextIn = out[predecessor];
                        firstPredecessor = false;
                    } else {
                        nextIn = intersectStates(nextIn, out[predecessor]);
                    }
                }
            }
            InitializationState nextOut = transferInitialization(
                block, nextIn, dataLayout, objects, references);
            EscapedObjects nextEscapedIn;
            if (&block != entry) {
                for (const llvm::BasicBlock* predecessor :
                     llvm::predecessors(&block)) {
                    if (reachable.count(predecessor) == 0)
                        continue;
                    nextEscapedIn.insert(escapedOut[predecessor].begin(),
                                         escapedOut[predecessor].end());
                }
            }
            EscapedObjects nextEscapedOut = transferEscapes(
                block, nextEscapedIn, dataLayout, objects, references);
            if (in[&block] != nextIn || out[&block] != nextOut ||
                escapedIn[&block] != nextEscapedIn ||
                escapedOut[&block] != nextEscapedOut) {
                in[&block] = std::move(nextIn);
                out[&block] = std::move(nextOut);
                escapedIn[&block] = std::move(nextEscapedIn);
                escapedOut[&block] = std::move(nextEscapedOut);
                changed = true;
            }
        }
    } while (changed);

    for (const llvm::BasicBlock& block : function) {
        if (reachable.count(&block) == 0)
            continue;
        InitializationState state = in[&block];
        EscapedObjects escaped = escapedIn[&block];
        for (const llvm::Instruction& instruction : block) {
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                reportUninitializedRead(
                    instruction, load->getPointerOperand(),
                    typeStoreSize(load->getType(), dataLayout), state,
                    dataLayout, objects, references, escaped, input, function,
                    findings);
            } else if (const auto* transfer =
                           llvm::dyn_cast<llvm::MemTransferInst>(
                               &instruction)) {
                const auto length =
                    memoryOperationLength(transfer->getLength());
                if (length)
                    reportUninitializedRead(
                        instruction, transfer->getSource(), *length, state,
                        dataLayout, objects, references, escaped, input,
                        function, findings);
            } else if (const auto* call =
                           llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                const auto* callee = llvm::dyn_cast<llvm::Function>(
                    call->getCalledOperand()->stripPointerCasts());
                if (callee &&
                    (simpleFunctionName(*callee) == "memcpy" ||
                     simpleFunctionName(*callee) == "memmove") &&
                    call->arg_size() >= 3) {
                    const auto length =
                        memoryOperationLength(call->getArgOperand(2));
                    if (length)
                        reportUninitializedRead(
                            instruction, call->getArgOperand(1), *length,
                            state, dataLayout, objects, references, escaped,
                            input, function, findings);
                }
            }
            applyMemoryWrites(instruction, state, dataLayout, objects,
                              references);
            EscapedObjects instructionEscapes = escapedByInstruction(
                instruction, dataLayout, objects, references);
            escaped.insert(instructionEscapes.begin(),
                           instructionEscapes.end());
        }
    }
}

} // namespace

void analyzeUninitialized(const llvm::Module& module,
                          const std::string& input,
                          std::vector<Finding>& findings) {
    for (const llvm::Function& function : module) {
        if (!function.isDeclaration())
            analyzeUninitializedReads(function, module.getDataLayout(), input,
                                      findings);
    }
}

} // namespace buganalyzer
