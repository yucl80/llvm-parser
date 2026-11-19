#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace buganalyzer {
namespace {

enum class ResourceKind {
    Malloc,
    New,
    NewArray,
    FileStream,
    PipeStream,
    FileDescriptor,
    Socket,
    WindowsHandle,
    DirectoryStream,
    DynamicLibrary,
    MappedMemory
};

using ResourceId = const llvm::CallBase*;
using ResourceSet = std::set<ResourceId>;
using ResourceAliases =
    std::unordered_map<const llvm::Value*, ResourceId>;
using ResourceSummaries =
    std::unordered_map<const llvm::Function*, ResourceKind>;

struct ReleaseEffect {
    unsigned argumentIndex;
    std::string releaseName;
};

using ReleaseSummaries =
    std::unordered_map<const llvm::Function*, std::vector<ReleaseEffect>>;

struct ResourceInfo {
    ResourceKind kind;
    std::string acquisitionName;
};

struct ResourceState {
    ResourceSet live;
    ResourceSet released;
    ResourceSet transferred;

    friend bool operator==(const ResourceState& left,
                           const ResourceState& right) {
        return left.live == right.live && left.released == right.released &&
               left.transferred == right.transferred;
    }

    friend bool operator!=(const ResourceState& left,
                           const ResourceState& right) {
        return !(left == right);
    }
};

const llvm::Function* resolveDirectFunction(const llvm::Value* value,
                                            unsigned depth = 0);

const llvm::Function* resolveDirectFunction(const llvm::Value* value,
                                            unsigned depth) {
    if (!value || depth > 16)
        return nullptr;
    value = value->stripPointerCasts();
    if (const auto* function = llvm::dyn_cast<llvm::Function>(value))
        return function;
    if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
        if (global->hasInitializer())
            return resolveDirectFunction(global->getInitializer(), depth + 1);
    }
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        const llvm::Instruction* reachingStore = nullptr;
        for (const llvm::Instruction& instruction : *load->getParent()) {
            if (&instruction == load)
                break;
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (store &&
                store->getPointerOperand()->stripPointerCasts() ==
                    load->getPointerOperand()->stripPointerCasts())
                reachingStore = store;
        }
        if (const auto* store =
                llvm::dyn_cast_or_null<llvm::StoreInst>(reachingStore))
            return resolveDirectFunction(store->getValueOperand(), depth + 1);
    }
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        const llvm::Function* common = nullptr;
        for (const llvm::Value* incoming : phi->incoming_values()) {
            const llvm::Function* candidate =
                resolveDirectFunction(incoming, depth + 1);
            if (!candidate || (common && candidate != common))
                return nullptr;
            common = candidate;
        }
        return common;
    }
    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        const llvm::Function* left =
            resolveDirectFunction(select->getTrueValue(), depth + 1);
        const llvm::Function* right =
            resolveDirectFunction(select->getFalseValue(), depth + 1);
        return left && left == right ? left : nullptr;
    }
    return nullptr;
}

std::string calleeName(const llvm::CallBase& call) {
    const llvm::Function* callee =
        resolveDirectFunction(call.getCalledOperand());
    return callee ? simpleFunctionName(*callee) : std::string();
}

const llvm::Function* calleeFunction(const llvm::CallBase& call) {
    return resolveDirectFunction(call.getCalledOperand());
}

bool classifyAcquisition(const llvm::CallBase& call, ResourceKind& kind,
                         std::string& name,
                         const ResourceSummaries& summaries) {
    name = calleeName(call);
    if (name == "operator new") {
        kind = ResourceKind::New;
    } else if (name == "operator new[]") {
        kind = ResourceKind::NewArray;
    } else if (isAllocationFunction(name) || name == "strdup" ||
               name == "strndup" || name == "realloc" ||
               name == "reallocarray") {
        kind = ResourceKind::Malloc;
    } else if (name == "mmap" || name == "mmap64") {
        kind = ResourceKind::MappedMemory;
    } else if (name == "dlopen") {
        kind = ResourceKind::DynamicLibrary;
    } else if (name == "opendir") {
        kind = ResourceKind::DirectoryStream;
    } else if (name == "fopen" || name == "freopen" ||
               name == "tmpfile" || name == "fdopen") {
        kind = ResourceKind::FileStream;
    } else if (name == "popen") {
        kind = ResourceKind::PipeStream;
    } else if (name == "open" || name == "open64" || name == "openat" ||
               name == "creat" || name == "dup" || name == "dup2" ||
               name == "dup3" || name == "accept" || name == "accept4" ||
               name == "mkstemp" || name == "mkstemp64" ||
               name == "shm_open") {
        kind = ResourceKind::FileDescriptor;
    } else if (name == "socket") {
        kind = ResourceKind::Socket;
    } else if (name == "CreateFileA" || name == "CreateFileW" ||
               name == "CreateFile2" || name == "CreateEventA" ||
               name == "CreateEventW" || name == "CreateMutexA" ||
               name == "CreateMutexW") {
        kind = ResourceKind::WindowsHandle;
    } else {
        const llvm::Function* callee = calleeFunction(call);
        const auto summary = summaries.find(callee);
        if (summary == summaries.end())
            return false;
        kind = summary->second;
    }
    return true;
}

bool isReleaseFunction(const std::string& name) {
    return name == "free" || name == "__libc_free" ||
           name == "operator delete" || name == "operator delete[]" ||
           name == "fclose" || name == "pclose" || name == "close" ||
           name == "closesocket" || name == "CloseHandle" ||
           name == "munmap" || name == "munmap64" ||
           name == "dlclose" || name == "closedir";
}

bool releaseMatches(const std::string& name, ResourceKind kind) {
    if (name == "free" || name == "__libc_free")
        return kind == ResourceKind::Malloc;
    if (name == "munmap" || name == "munmap64")
        return kind == ResourceKind::MappedMemory;
    if (name == "operator delete")
        return kind == ResourceKind::New;
    if (name == "operator delete[]")
        return kind == ResourceKind::NewArray;
    if (name == "fclose")
        return kind == ResourceKind::FileStream;
    if (name == "pclose")
        return kind == ResourceKind::PipeStream;
    if (name == "close")
        return kind == ResourceKind::FileDescriptor ||
               kind == ResourceKind::Socket;
    if (name == "closesocket")
        return kind == ResourceKind::Socket;
    if (name == "CloseHandle")
        return kind == ResourceKind::WindowsHandle;
    if (name == "closedir")
        return kind == ResourceKind::DirectoryStream;
    if (name == "dlclose")
        return kind == ResourceKind::DynamicLibrary;
    return false;
}

std::string resourceName(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Malloc: return "heap allocation";
    case ResourceKind::New: return "scalar new allocation";
    case ResourceKind::NewArray: return "array new allocation";
    case ResourceKind::FileStream: return "FILE stream";
    case ResourceKind::PipeStream: return "process pipe";
    case ResourceKind::FileDescriptor: return "file descriptor";
    case ResourceKind::Socket: return "socket";
    case ResourceKind::WindowsHandle: return "Windows handle";
    case ResourceKind::DirectoryStream: return "directory stream";
    case ResourceKind::DynamicLibrary: return "dynamic library handle";
    case ResourceKind::MappedMemory: return "mapped memory";
    }
    return "resource";
}

const char* leakCwe(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Malloc:
    case ResourceKind::New:
    case ResourceKind::NewArray:
    case ResourceKind::MappedMemory:
        return "CWE-401";
    case ResourceKind::FileStream:
    case ResourceKind::PipeStream:
    case ResourceKind::FileDescriptor:
    case ResourceKind::Socket:
    case ResourceKind::WindowsHandle:
    case ResourceKind::DirectoryStream:
    case ResourceKind::DynamicLibrary:
        return "CWE-775";
    }
    return "CWE-401";
}

ResourceId resolveResource(const llvm::Value* value,
                           const ResourceAliases& aliases,
                           unsigned depth = 0) {
    if (!value || depth > 16)
        return nullptr;
    if (value->getType()->isPointerTy())
        value = value->stripPointerCasts();
    const auto found = aliases.find(value);
    if (found != aliases.end())
        return found->second;

    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
        return resolveResource(gep->getPointerOperand(), aliases, depth + 1);
    if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
        return resolveResource(cast->getOperand(0), aliases, depth + 1);
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        ResourceId common = nullptr;
        for (const llvm::Value* incoming : phi->incoming_values()) {
            ResourceId resource =
                resolveResource(incoming, aliases, depth + 1);
            if (!resource || (common && resource != common))
                return nullptr;
            common = resource;
        }
        return common;
    }
    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        ResourceId left =
            resolveResource(select->getTrueValue(), aliases, depth + 1);
        ResourceId right =
            resolveResource(select->getFalseValue(), aliases, depth + 1);
        return left && left == right ? left : nullptr;
    }
    return nullptr;
}

ResourceAliases buildResourceAliases(
    const llvm::Function& function,
    const std::unordered_map<ResourceId, ResourceInfo>& resources) {
    ResourceAliases aliases;
    for (const auto& entry : resources)
        aliases.emplace(entry.first, entry.first);

    std::map<const llvm::AllocaInst*, ResourceSet> slotResources;
    std::set<const llvm::LoadInst*> resourceLoads;
    std::set<const llvm::AllocaInst*> ambiguousSlots;
    bool changed;
    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                if (const auto* store =
                        llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    const llvm::Value* stored = store->getValueOperand();
                    if (!stored->getType()->isPointerTy() &&
                        !stored->getType()->isIntegerTy())
                        continue;
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        store->getPointerOperand()->stripPointerCasts());
                    ResourceId resource = resolveResource(stored, aliases);
                    if (slot) {
                        if (resource) {
                            if (slotResources[slot].insert(resource).second)
                                changed = true;
                        } else {
                            const auto* integer =
                                llvm::dyn_cast<llvm::ConstantInt>(stored);
                            if (!integer ||
                                (!integer->isMinusOne() && !integer->isZero())) {
                                if (ambiguousSlots.insert(slot).second)
                                    changed = true;
                            }
                        }
                    }
                } else if (const auto* load =
                               llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    if (!load->getType()->isPointerTy() &&
                        !load->getType()->isIntegerTy())
                        continue;
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        load->getPointerOperand()->stripPointerCasts());
                    if (!slot)
                        continue;
                    resourceLoads.insert(load);
                } else if (const auto* cast =
                               llvm::dyn_cast<llvm::CastInst>(&instruction)) {
                    ResourceId resource =
                        resolveResource(cast->getOperand(0), aliases);
                    if (resource && aliases.emplace(cast, resource).second)
                        changed = true;
                } else if (const auto* gep =
                               llvm::dyn_cast<llvm::GetElementPtrInst>(
                                   &instruction)) {
                    ResourceId resource =
                        resolveResource(gep->getPointerOperand(), aliases);
                    if (resource && aliases.emplace(gep, resource).second)
                        changed = true;
                }
            }
        }
    } while (changed);

    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (!store)
                    continue;
                const llvm::Value* stored = store->getValueOperand();
                if (!stored->getType()->isPointerTy() &&
                    !stored->getType()->isIntegerTy())
                    continue;
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    store->getPointerOperand()->stripPointerCasts());
                ResourceId resource = resolveResource(stored, aliases);
                if (slot) {
                    if (resource) {
                        if (slotResources[slot].insert(resource).second)
                            changed = true;
                    } else {
                        const auto* integer =
                            llvm::dyn_cast<llvm::ConstantInt>(stored);
                        if (!integer ||
                            (!integer->isMinusOne() && !integer->isZero())) {
                            if (ambiguousSlots.insert(slot).second)
                                changed = true;
                        }
                    }
                }
            }
        }
        for (const llvm::LoadInst* load : resourceLoads) {
            if (aliases.count(load) != 0)
                continue;
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                load->getPointerOperand()->stripPointerCasts());
            if (!slot)
                continue;

            const llvm::StoreInst* reachingStore = nullptr;
            for (const llvm::Instruction& instruction : *load->getParent()) {
                if (&instruction == load)
                    break;
                const auto* store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (store &&
                    store->getPointerOperand()->stripPointerCasts() == slot)
                    reachingStore = store;
            }

            ResourceId resource = nullptr;
            if (reachingStore) {
                resource =
                    resolveResource(reachingStore->getValueOperand(), aliases);
            } else if (ambiguousSlots.count(slot) == 0) {
                const auto found = slotResources.find(slot);
                if (found != slotResources.end() && found->second.size() == 1)
                    resource = *found->second.begin();
            }
            if (resource && aliases.emplace(load, resource).second)
                changed = true;
        }
    } while (changed);

    return aliases;
}

std::unordered_map<ResourceId, ResourceInfo> collectResources(
    const llvm::Function& function, const ResourceSummaries& summaries) {
    std::unordered_map<ResourceId, ResourceInfo> resources;
    for (const llvm::BasicBlock& block : function) {
        for (const llvm::Instruction& instruction : block) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call)
                continue;
            ResourceKind kind;
            std::string name;
            if (classifyAcquisition(*call, kind, name, summaries))
                resources.emplace(call, ResourceInfo{kind, std::move(name)});
        }
    }
    return resources;
}

bool summarizeReturnedResource(
    const llvm::Function& function, const ResourceSummaries& summaries,
    ResourceKind& kind) {
    const auto resources = collectResources(function, summaries);
    if (resources.empty())
        return false;
    const ResourceAliases aliases = buildResourceAliases(function, resources);

    bool foundResource = false;
    bool foundFailureSentinel = false;
    for (const llvm::BasicBlock& block : function) {
        const auto* returned =
            llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        if (!returned || !returned->getReturnValue())
            continue;
        const llvm::Value* value = returned->getReturnValue();
        const auto* constant = llvm::dyn_cast<llvm::Constant>(value);
        if (constant && value->getType()->isPointerTy() &&
            constant->isNullValue())
            continue;
        if (const auto* integer =
                llvm::dyn_cast_or_null<llvm::ConstantInt>(constant)) {
            if (integer->isMinusOne()) {
                foundFailureSentinel = true;
                continue;
            }
        }
        ResourceId resource = resolveResource(value, aliases);
        const auto info = resources.find(resource);
        if (info == resources.end()) {
            if (resources.size() == 1 &&
                llvm::isa<llvm::LoadInst>(value) &&
                value->getType()->isIntegerTy()) {
                kind = resources.begin()->second.kind;
                foundResource = true;
                continue;
            }
            return false;
        }
        if (foundResource && kind != info->second.kind)
            return false;
        kind = info->second.kind;
        foundResource = true;
    }
    return foundResource &&
           (!foundFailureSentinel ||
            kind == ResourceKind::FileDescriptor ||
            kind == ResourceKind::Socket);
}

ResourceSummaries buildResourceSummaries(const llvm::Module& module) {
    ResourceSummaries summaries;
    bool changed;
    do {
        changed = false;
        for (const llvm::Function& function : module) {
            if (function.isDeclaration() || summaries.count(&function) != 0)
                continue;
            ResourceKind kind;
            if (summarizeReturnedResource(function, summaries, kind)) {
                summaries.emplace(&function, kind);
                changed = true;
            }
        }
    } while (changed);
    return summaries;
}

bool exitsFunction(const llvm::Instruction& instruction);

ReleaseSummaries buildReleaseSummaries(const llvm::Module& module) {
    ReleaseSummaries summaries;
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;

        std::map<const llvm::AllocaInst*, std::set<unsigned>> slotParameters;
        bool changed;
        do {
            changed = false;
            for (const llvm::BasicBlock& block : function) {
                for (const llvm::Instruction& instruction : block) {
                    const auto* store =
                        llvm::dyn_cast<llvm::StoreInst>(&instruction);
                    if (!store)
                        continue;
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        store->getPointerOperand()->stripPointerCasts());
                    if (!slot)
                        continue;
                    const llvm::Value* stored =
                        store->getValueOperand()->stripPointerCasts();
                    if (const auto* argument =
                            llvm::dyn_cast<llvm::Argument>(stored)) {
                        if (argument->getParent() == &function &&
                            slotParameters[slot].insert(argument->getArgNo()).second)
                            changed = true;
                    } else if (const auto* load =
                                   llvm::dyn_cast<llvm::LoadInst>(stored)) {
                        const auto* source = llvm::dyn_cast<llvm::AllocaInst>(
                            load->getPointerOperand()->stripPointerCasts());
                        const auto sourceParameters = slotParameters.find(source);
                        if (!source || sourceParameters == slotParameters.end())
                            continue;
                        const std::size_t oldSize = slotParameters[slot].size();
                        slotParameters[slot].insert(
                            sourceParameters->second.begin(),
                            sourceParameters->second.end());
                        changed |= slotParameters[slot].size() != oldSize;
                    }
                }
            }
        } while (changed);

        using ReleaseCall = std::pair<const llvm::CallBase*, std::string>;
        std::unordered_map<unsigned, std::vector<ReleaseCall>> effects;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !isReleaseFunction(calleeName(*call)) ||
                    call->arg_size() == 0)
                    continue;
                const llvm::Value* value =
                    call->getArgOperand(0)->stripPointerCasts();
                if (const auto* argument =
                        llvm::dyn_cast<llvm::Argument>(value)) {
                    if (argument->getParent() == &function)
                        effects[argument->getArgNo()].emplace_back(
                            call, calleeName(*call));
                } else if (const auto* load =
                               llvm::dyn_cast<llvm::LoadInst>(value)) {
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        load->getPointerOperand()->stripPointerCasts());
                    const auto parameters = slotParameters.find(slot);
                    if (parameters != slotParameters.end() &&
                        parameters->second.size() == 1)
                        effects[*parameters->second.begin()].emplace_back(
                            call, calleeName(*call));
                }
            }
        }

        llvm::DominatorTree dominatorTree(
            const_cast<llvm::Function&>(function));
        std::vector<ReleaseEffect> functionEffects;
        for (const auto& entry : effects) {
            const std::string& releaseName = entry.second.front().second;
            const bool consistentRelease = std::all_of(
                entry.second.begin(), entry.second.end(),
                [&releaseName](const ReleaseCall& effect) {
                    return effect.second == releaseName;
                });
            if (!consistentRelease)
                continue;

            bool hasExit = false;
            bool everyExitReleases = true;
            for (const llvm::BasicBlock& block : function) {
                const llvm::Instruction* terminator = block.getTerminator();
                if (!exitsFunction(*terminator))
                    continue;
                hasExit = true;
                const bool releasedOnEveryPath = std::any_of(
                    entry.second.begin(), entry.second.end(),
                    [&dominatorTree, terminator](const ReleaseCall& effect) {
                        return dominatorTree.dominates(effect.first,
                                                       terminator);
                    });
                if (!releasedOnEveryPath) {
                    everyExitReleases = false;
                    break;
                }
            }
            if (hasExit && everyExitReleases)
                functionEffects.push_back(
                    ReleaseEffect{entry.first, releaseName});
        }
        if (!functionEffects.empty())
            summaries.emplace(&function, std::move(functionEffects));
    }
    return summaries;
}

bool isOwnerTransfer(const llvm::CallBase& call) {
    const llvm::Function* callee = calleeFunction(call);
    if (!callee)
        return false;
    const std::string demangled = llvm::demangle(callee->getName().str());
    const std::string directName = calleeName(call);
    if (demangled.find("take_ownership") != std::string::npos ||
        demangled.find("transfer_ownership") != std::string::npos)
        return true;
    const std::string name = directName.empty()
                                ? simpleFunctionName(*callee)
                                : directName;
    if (name.find("unique_ptr") != std::string::npos ||
        name.find("shared_ptr") != std::string::npos ||
        name.find("weak_ptr") != std::string::npos ||
        name.find("auto_ptr") != std::string::npos ||
        name.find("scoped_ptr") != std::string::npos)
        return true;
    return false;
}

bool storesToGlobal(const llvm::StoreInst& store) {
    return llvm::isa<llvm::GlobalVariable>(
        llvm::getUnderlyingObject(store.getPointerOperand()));
}

void mergeResourceSets(ResourceSet& destination, const ResourceSet& source) {
    destination.insert(source.begin(), source.end());
}

ResourceState mergeStates(const ResourceState& left,
                          const ResourceState& right) {
    ResourceState result = left;
    mergeResourceSets(result.live, right.live);
    mergeResourceSets(result.released, right.released);
    mergeResourceSets(result.transferred, right.transferred);
    return result;
}

std::vector<std::pair<ResourceId, std::string>> releaseEffectsForCall(
    const llvm::CallBase& call, const ResourceAliases& aliases,
    const ReleaseSummaries& summaries) {
    std::vector<std::pair<ResourceId, std::string>> effects;
    const std::string name = calleeName(call);
    if (isReleaseFunction(name) && call.arg_size() > 0) {
        ResourceId resource =
            resolveResource(call.getArgOperand(0), aliases);
        if (resource)
            effects.emplace_back(resource, name);
    }

    const auto* callee = calleeFunction(call);
    const auto summary = summaries.find(callee);
    if (summary != summaries.end()) {
        for (const ReleaseEffect& effect : summary->second) {
            if (effect.argumentIndex >= call.arg_size())
                continue;
            ResourceId resource = resolveResource(
                call.getArgOperand(effect.argumentIndex), aliases);
            if (resource)
                effects.emplace_back(resource, effect.releaseName);
        }
    }
    return effects;
}

bool transfersResourceInInstruction(
    const llvm::Instruction& instruction, ResourceId resource,
    const ResourceAliases& aliases, const ReleaseSummaries& releaseSummaries) {
    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        return resolveResource(store->getValueOperand(), aliases) == resource &&
               storesToGlobal(*store);
    if (const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
        return resolveResource(returned->getReturnValue(), aliases) == resource;
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call)
        return false;
    if (calleeName(*call) == "fdopen" && call->arg_size() > 0 &&
        resolveResource(call->getArgOperand(0), aliases) == resource)
        return true;
    for (const auto& effect :
         releaseEffectsForCall(*call, aliases, releaseSummaries)) {
        if (effect.first == resource)
            return true;
    }
    if (isOwnerTransfer(*call)) {
        for (const llvm::Use& argument : call->args()) {
            if (resolveResource(argument.get(), aliases) == resource)
                return true;
        }
    }
    return false;
}

bool exitsFunction(const llvm::Instruction& instruction) {
    if (llvm::isa<llvm::ReturnInst>(instruction) ||
        llvm::isa<llvm::ResumeInst>(instruction))
        return true;
    if (const auto* cleanup =
            llvm::dyn_cast<llvm::CleanupReturnInst>(&instruction))
        return cleanup->unwindsToCaller();
    if (const auto* handlers =
            llvm::dyn_cast<llvm::CatchSwitchInst>(&instruction))
        return handlers->unwindsToCaller();
    return false;
}

std::unordered_set<const llvm::BasicBlock*> findReachableBlocks(
    const llvm::Function& function) {
    std::unordered_set<const llvm::BasicBlock*> reachable;
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

void findLoopAllocationCandidates(
    llvm::Loop& loop, ResourceSet& candidates,
    const std::unordered_map<ResourceId, ResourceInfo>& resources,
    const ResourceAliases& aliases,
    const ReleaseSummaries& releaseSummaries,
    llvm::DominatorTree& dominatorTree) {
    for (llvm::BasicBlock* block : loop.blocks()) {
        for (const llvm::Instruction& instruction : *block) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call || resources.find(call) == resources.end())
                continue;

            bool handledInIteration = false;
            llvm::BasicBlock* latch = loop.getLoopLatch();
            for (llvm::BasicBlock* candidateBlock : loop.blocks()) {
                for (const llvm::Instruction& candidate :
                     *candidateBlock) {
                    if (&candidate != call &&
                        dominatorTree.dominates(call, &candidate) &&
                        latch &&
                        dominatorTree.dominates(&candidate,
                                                latch->getTerminator()) &&
                        transfersResourceInInstruction(
                            candidate, call, aliases, releaseSummaries)) {
                        handledInIteration = true;
                        break;
                    }
                }
                if (handledInIteration)
                    break;
            }
            if (!handledInIteration)
                candidates.insert(call);
        }
    }
    for (llvm::Loop* subLoop : loop.getSubLoops())
        findLoopAllocationCandidates(*subLoop, candidates, resources, aliases,
                                     releaseSummaries, dominatorTree);
}

ResourceSet findLoopAllocationCandidates(
    const llvm::Function& function,
    const std::unordered_map<ResourceId, ResourceInfo>& resources,
    const ResourceAliases& aliases,
    const ReleaseSummaries& releaseSummaries) {
    llvm::DominatorTree dominatorTree(
        const_cast<llvm::Function&>(function));
    llvm::LoopInfo loopInfo(dominatorTree);
    ResourceSet candidates;
    for (llvm::Loop* loop : loopInfo)
        findLoopAllocationCandidates(*loop, candidates, resources, aliases,
                                     releaseSummaries, dominatorTree);
    return candidates;
}

void transferInstruction(const llvm::Instruction& instruction,
                         ResourceState& state,
                         const std::unordered_map<ResourceId, ResourceInfo>&
                             resources,
                         const ResourceAliases& aliases,
                         const ReleaseSummaries& releaseSummaries) {
    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        ResourceId resource =
            resolveResource(store->getValueOperand(), aliases);
        if (resource && storesToGlobal(*store)) {
            state.live.erase(resource);
            state.transferred.insert(resource);
        }
        return;
    }

    if (const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
        ResourceId resource =
            resolveResource(returned->getReturnValue(), aliases);
        if (resource) {
            state.live.erase(resource);
            state.transferred.insert(resource);
        }
        return;
    }

    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call)
        return;
    const std::string name = calleeName(*call);

    if (name == "fdopen" && call->arg_size() > 0) {
        ResourceId descriptor =
            resolveResource(call->getArgOperand(0), aliases);
        const auto info = resources.find(descriptor);
        if (info != resources.end() &&
            info->second.kind == ResourceKind::FileDescriptor) {
            state.live.erase(descriptor);
            state.transferred.insert(descriptor);
        }
    }

    for (const auto& effect :
         releaseEffectsForCall(*call, aliases, releaseSummaries)) {
        ResourceId resource = effect.first;
        if (resources.find(resource) != resources.end()) {
            state.live.erase(resource);
            state.released.insert(resource);
            state.transferred.erase(resource);
        }
    }

    if (isOwnerTransfer(*call)) {
        for (const llvm::Use& argument : call->args()) {
            ResourceId resource = resolveResource(argument.get(), aliases);
            if (resource) {
                state.live.erase(resource);
                state.transferred.insert(resource);
            }
        }
        if (state.live.size() == 1 && state.transferred.empty()) {
            ResourceId sole = *state.live.begin();
            state.live.erase(sole);
            state.transferred.insert(sole);
        }
    }

    const auto acquisition = resources.find(call);
    if (acquisition != resources.end()) {
        state.live.insert(call);
        state.released.erase(call);
        state.transferred.erase(call);
    }
}

ResourceState transferBlock(
    const llvm::BasicBlock& block, ResourceState state,
    const std::unordered_map<ResourceId, ResourceInfo>& resources,
    const ResourceAliases& aliases,
    const ReleaseSummaries& releaseSummaries) {
    for (const llvm::Instruction& instruction : block)
        transferInstruction(instruction, state, resources, aliases,
                            releaseSummaries);
    return state;
}

bool isSentinelPointerValue(const llvm::Value* value) {
    if (!value)
        return false;
    value = value->stripPointerCasts();
    if (const auto* constant = llvm::dyn_cast<llvm::Constant>(value))
        if (constant->isNullValue())
            return true;
    if (const auto* expression =
            llvm::dyn_cast<llvm::ConstantExpr>(value)) {
        if (expression->getOpcode() == llvm::Instruction::IntToPtr &&
            expression->getNumOperands() > 0) {
            if (const auto* integer = llvm::dyn_cast<llvm::ConstantInt>(
                    expression->getOperand(0)))
                return integer->isMinusOne();
        }
    }
    return false;
}

ResourceState stateForEdge(
    const llvm::BasicBlock& predecessor, const llvm::BasicBlock& successor,
    ResourceState state,
    const std::unordered_map<ResourceId, ResourceInfo>& resources,
    const ResourceAliases& aliases) {
    const auto* branch =
        llvm::dyn_cast<llvm::BranchInst>(predecessor.getTerminator());
    if (!branch || !branch->isConditional())
        return state;
    const auto* comparison =
        llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
    if (!comparison)
        return state;

    const llvm::Value* resourceValue = nullptr;
    const llvm::Value* nullValue = nullptr;
    for (unsigned index = 0; index != 2; ++index) {
        const llvm::Value* operand = comparison->getOperand(index);
        const auto* constant = llvm::dyn_cast<llvm::Constant>(operand);
        if (isSentinelPointerValue(operand) ||
            (constant &&
             llvm::dyn_cast<llvm::ConstantInt>(operand) &&
             llvm::dyn_cast<llvm::ConstantInt>(operand)->isMinusOne())) {
            nullValue = operand;
            resourceValue = comparison->getOperand(1 - index);
            break;
        }
        if (const auto* integer =
                llvm::dyn_cast<llvm::ConstantInt>(operand)) {
            if (integer->isMinusOne()) {
                nullValue = operand;
                resourceValue = comparison->getOperand(1 - index);
                break;
            }
            if (integer->isZero() &&
                comparison->getOperand(1 - index)->getType()->isIntegerTy()) {
                nullValue = operand;
                resourceValue = comparison->getOperand(1 - index);
                break;
            }
        }
    }
    if (!nullValue || !resourceValue ||
        (!resourceValue->getType()->isPointerTy() &&
         !resourceValue->getType()->isIntegerTy()))
        return state;

    ResourceId resource = resolveResource(resourceValue, aliases);
    if (!resource || resources.find(resource) == resources.end())
        return state;
    const llvm::CmpInst::Predicate predicate = comparison->getPredicate();
    const bool integerResource =
        resourceValue->getType()->isIntegerTy();
    if (predicate != llvm::CmpInst::ICMP_EQ &&
        predicate != llvm::CmpInst::ICMP_NE &&
        !(integerResource &&
          (predicate == llvm::CmpInst::ICMP_SLT ||
           predicate == llvm::CmpInst::ICMP_SLE ||
           predicate == llvm::CmpInst::ICMP_SGT ||
           predicate == llvm::CmpInst::ICMP_SGE)))
        return state;

    const bool trueEdge = branch->getSuccessor(0) == &successor;
    bool nullEdge = false;
    if (predicate == llvm::CmpInst::ICMP_EQ ||
        predicate == llvm::CmpInst::ICMP_NE) {
        nullEdge =
            (predicate == llvm::CmpInst::ICMP_EQ) == trueEdge;
    } else if (integerResource) {
        const bool zeroSentinel =
            llvm::isa<llvm::ConstantInt>(nullValue) &&
            llvm::cast<llvm::ConstantInt>(nullValue)->isZero();
        if (predicate == llvm::CmpInst::ICMP_SLT && zeroSentinel)
            nullEdge = trueEdge;
        else if (predicate == llvm::CmpInst::ICMP_SLE)
            nullEdge = trueEdge;
        else if (predicate == llvm::CmpInst::ICMP_SGT && !zeroSentinel)
            nullEdge = !trueEdge;
        else if (predicate == llvm::CmpInst::ICMP_SGE && zeroSentinel)
            nullEdge = !trueEdge;
    }
    if (calleeName(*resource) == "realloc" ||
        calleeName(*resource) == "reallocarray") {
        ResourceId previous = nullptr;
        if (resource->arg_size() > 0)
            previous =
                resolveResource(resource->getArgOperand(0), aliases);
        if (nullEdge) {
            state.live.erase(resource);
            state.released.erase(resource);
            state.transferred.erase(resource);
        } else if (previous) {
            state.live.erase(previous);
            state.released.insert(previous);
            state.transferred.erase(previous);
        }
        return state;
    }
    if (nullEdge) {
        state.live.erase(resource);
        state.released.erase(resource);
        state.transferred.erase(resource);
    }
    return state;
}

void reportResourceEvents(
    const llvm::Function& function, const std::string& input,
    const std::unordered_map<ResourceId, ResourceInfo>& resources,
    const ResourceAliases& aliases,
    const ReleaseSummaries& releaseSummaries,
    const std::unordered_map<const llvm::BasicBlock*, ResourceState>& inStates,
    std::vector<Finding>& findings) {
    std::unordered_set<ResourceId> reportedLeaks;
    const ResourceSet loopLeakCandidates = findLoopAllocationCandidates(
        function, resources, aliases, releaseSummaries);
    const std::unordered_set<const llvm::BasicBlock*> reachable =
        findReachableBlocks(function);
    for (const llvm::BasicBlock& block : function) {
        if (reachable.count(&block) == 0)
            continue;
        ResourceState state = inStates.at(&block);
        for (const llvm::Instruction& instruction : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                if (loopLeakCandidates.count(call) != 0 &&
                    reportedLeaks.insert(call).second) {
                    const auto info = resources.find(call);
                    if (info != resources.end()) {
                        addFinding(
                            findings, input, function, instruction,
                            "RESOURCE_LEAK", leakCwe(info->second.kind),
                            "medium", "resource-lifetime",
                            info->second.acquisitionName +
                                " executes in a loop without a release or "
                                "ownership transfer in the same basic block; "
                                "earlier iterations may leak when the "
                                "allocation result is overwritten.");
                    }
                }
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                for (const auto& effect :
                     releaseEffectsForCall(*call, aliases, releaseSummaries)) {
                    ResourceId resource = effect.first;
                    const std::string& name = effect.second;
                    const auto info = resources.find(resource);
                    if (info != resources.end()) {
                        if (!releaseMatches(name, info->second.kind)) {
                            addFinding(
                                findings, input, function, instruction,
                                "RESOURCE_MISMATCH", "CWE-762", "high",
                                "resource-lifetime",
                                "Resource acquired by " +
                                    info->second.acquisitionName +
                                    " (" + resourceName(info->second.kind) +
                                    ") is released by incompatible function " +
                                    name + ".");
                        }
                        if (state.released.count(resource) != 0 &&
                            state.live.count(resource) == 0) {
                            addFinding(
                                findings, input, function, instruction,
                                "RESOURCE_DOUBLE_RELEASE", "CWE-415", "high",
                                "resource-lifetime",
                                "Resource acquired by " +
                                    info->second.acquisitionName +
                                    " may be released more than once.");
                        }
                    }
                }
            }

            if (exitsFunction(instruction)) {
                ResourceId returnedResource = nullptr;
                if (const auto* returned =
                        llvm::dyn_cast<llvm::ReturnInst>(&instruction))
                    returnedResource =
                        resolveResource(returned->getReturnValue(), aliases);
                if (!returnedResource &&
                    resources.size() == 1 &&
                    llvm::isa<llvm::ReturnInst>(&instruction)) {
                    const auto* returned =
                        llvm::cast<llvm::ReturnInst>(&instruction);
                    if (returned->getReturnValue() &&
                        llvm::isa<llvm::LoadInst>(returned->getReturnValue()) &&
                        returned->getReturnValue()->getType()->isIntegerTy()) {
                        returnedResource = resources.begin()->first;
                    }
                }
                for (ResourceId resource : state.live) {
                    if (resource == returnedResource ||
                        !reportedLeaks.insert(resource).second)
                        continue;
                    const auto info = resources.find(resource);
                    if (info == resources.end())
                        continue;
                    addFinding(
                        findings, input, function, *resource, "RESOURCE_LEAK",
                        leakCwe(info->second.kind), "medium",
                        "resource-lifetime",
                        info->second.acquisitionName + " acquires a " +
                            resourceName(info->second.kind) +
                            " that may remain unreleased when this function "
                            "exits.");
                }
            }

            transferInstruction(instruction, state, resources, aliases,
                                releaseSummaries);
        }
    }
}

void analyzeFunction(const llvm::Function& function,
                     const std::string& input,
                     const ResourceSummaries& resourceSummaries,
                     const ReleaseSummaries& releaseSummaries,
                     std::vector<Finding>& findings) {
    const auto resources = collectResources(function, resourceSummaries);
    if (resources.empty())
        return;

    const ResourceAliases aliases = buildResourceAliases(function, resources);
    std::unordered_map<const llvm::BasicBlock*, ResourceState> inStates;
    std::unordered_map<const llvm::BasicBlock*, ResourceState> outStates;
    const llvm::BasicBlock* entry = &function.getEntryBlock();
    const std::unordered_set<const llvm::BasicBlock*> reachable =
        findReachableBlocks(function);
    for (const llvm::BasicBlock& block : function) {
        inStates[&block] = {};
        outStates[&block] = {};
    }

    bool changed;
    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            if (reachable.count(&block) == 0)
                continue;
            ResourceState nextIn;
            if (&block != entry) {
                for (const llvm::BasicBlock* predecessor :
                     llvm::predecessors(&block)) {
                    if (reachable.count(predecessor) == 0)
                        continue;
                    ResourceState edge = stateForEdge(
                        *predecessor, block, outStates[predecessor], resources,
                        aliases);
                    nextIn = mergeStates(nextIn, edge);
                }
            }
            ResourceState nextOut =
                transferBlock(block, nextIn, resources, aliases,
                              releaseSummaries);
            if (inStates[&block] != nextIn || outStates[&block] != nextOut) {
                inStates[&block] = std::move(nextIn);
                outStates[&block] = std::move(nextOut);
                changed = true;
            }
        }
    } while (changed);

    reportResourceEvents(function, input, resources, aliases, releaseSummaries,
                         inStates, findings);
}

} // namespace

void analyzeResourceLifetimes(const llvm::Module& module,
                              const std::string& input,
                              std::vector<Finding>& findings) {
    const ResourceSummaries resourceSummaries =
        buildResourceSummaries(module);
    const ReleaseSummaries releaseSummaries =
        buildReleaseSummaries(module);
    for (const llvm::Function& function : module) {
        if (!function.isDeclaration())
            analyzeFunction(function, input, resourceSummaries,
                            releaseSummaries, findings);
    }
}

} // namespace buganalyzer
