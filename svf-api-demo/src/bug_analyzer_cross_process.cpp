#include "bug_analyzer_plugin.h"

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "MemoryModel/PointsTo.h"
#include "Util/Casting.h"
#include "Util/Options.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace buganalyzer {
namespace {

enum class TriState { No, Yes, Unknown };
enum class CrossKind { Local, Remote };

using IndirectCallees = std::unordered_map<
    const llvm::CallBase*, std::vector<const llvm::Function*>>;

struct AddressFamilies {
    std::uint64_t unixDomain = 1;
    std::uint64_t inet = 2;
    std::uint64_t inet6 = 10;
};

struct ClassificationContext {
    AddressFamilies families;
    std::unordered_map<const llvm::Value*, CrossKind> socketKinds;
    std::unordered_map<const llvm::Function*, CrossKind> functionReturnKinds;
};

std::unordered_set<std::string> gLocalApis;
std::unordered_set<std::string> gRemoteApis;
std::mutex gLocalApiMutex;

std::vector<const llvm::Function*> collectCallees(
    const llvm::CallBase& call, const IndirectCallees& indirect);

const llvm::Function* resolveDirectFunction(const llvm::Value* value,
                                            unsigned depth = 0) {
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
            if (store && store->getPointerOperand()->stripPointerCasts() ==
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

bool evaluateConstant(const llvm::Value* value, llvm::APInt& result,
                      unsigned depth = 0);

bool evaluateOperator(unsigned opcode, const llvm::User& operands,
                      llvm::APInt& result, unsigned depth) {
    if (operands.getNumOperands() == 0)
        return false;

    llvm::APInt left;
    llvm::APInt right;
    switch (opcode) {
    case llvm::Instruction::Or:
    case llvm::Instruction::And:
    case llvm::Instruction::Xor:
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
        if (operands.getNumOperands() < 2 ||
            !evaluateConstant(operands.getOperand(0), left, depth + 1) ||
            !evaluateConstant(operands.getOperand(1), right, depth + 1) ||
            left.getBitWidth() != right.getBitWidth())
            return false;
        switch (opcode) {
        case llvm::Instruction::Or: result = left | right; return true;
        case llvm::Instruction::And: result = left & right; return true;
        case llvm::Instruction::Xor: result = left ^ right; return true;
        case llvm::Instruction::Add: result = left + right; return true;
        case llvm::Instruction::Sub: result = left - right; return true;
        case llvm::Instruction::Mul: result = left * right; return true;
        case llvm::Instruction::Shl:
            if (right.uge(left.getBitWidth()))
                return false;
            result = left.shl(right.getZExtValue());
            return true;
        case llvm::Instruction::LShr:
            if (right.uge(left.getBitWidth()))
                return false;
            result = left.lshr(right.getZExtValue());
            return true;
        case llvm::Instruction::AShr:
            if (right.uge(left.getBitWidth()))
                return false;
            result = left.ashr(right.getZExtValue());
            return true;
        default: return false;
        }

    case llvm::Instruction::ICmp: {
        if (operands.getNumOperands() < 2 ||
            !evaluateConstant(operands.getOperand(0), left, depth + 1) ||
            !evaluateConstant(operands.getOperand(1), right, depth + 1) ||
            left.getBitWidth() != right.getBitWidth())
            return false;
        bool value = false;
        switch (llvm::cast<llvm::ICmpInst>(operands).getPredicate()) {
        case llvm::CmpInst::ICMP_EQ: value = left.eq(right); break;
        case llvm::CmpInst::ICMP_NE: value = left.ne(right); break;
        case llvm::CmpInst::ICMP_UGT: value = left.ugt(right); break;
        case llvm::CmpInst::ICMP_UGE: value = left.uge(right); break;
        case llvm::CmpInst::ICMP_ULT: value = left.ult(right); break;
        case llvm::CmpInst::ICMP_ULE: value = left.ule(right); break;
        case llvm::CmpInst::ICMP_SGT: value = left.sgt(right); break;
        case llvm::CmpInst::ICMP_SGE: value = left.sge(right); break;
        case llvm::CmpInst::ICMP_SLT: value = left.slt(right); break;
        case llvm::CmpInst::ICMP_SLE: value = left.sle(right); break;
        default: return false;
        }
        result = llvm::APInt(1, value ? 1 : 0);
        return true;
    }

    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::Trunc:
    case llvm::Instruction::BitCast: {
        const unsigned width = operands.getType()->getIntegerBitWidth();
        if (width == 0 || width > 64 ||
            !evaluateConstant(operands.getOperand(0), left, depth + 1))
            return false;
        if (opcode == llvm::Instruction::ZExt)
            result = left.zext(width);
        else if (opcode == llvm::Instruction::SExt)
            result = left.sext(width);
        else if (opcode == llvm::Instruction::Trunc)
            result = left.trunc(width);
        else
            result = left;
        return true;
    }
    default:
        return false;
    }
}

bool evaluateConstant(const llvm::Value* value, llvm::APInt& result,
                      unsigned depth) {
    if (!value || depth > 16)
        return false;
    value = value->stripPointerCasts();
    if (const auto* integer = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        result = integer->getValue();
        return true;
    }
    if (const auto* expression = llvm::dyn_cast<llvm::ConstantExpr>(value))
        return evaluateOperator(expression->getOpcode(), *expression, result,
                                depth);
    if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value))
        return evaluateOperator(instruction->getOpcode(), *instruction, result,
                                depth);
    return false;
}

bool constantUInt(const llvm::Value* value, std::uint64_t& result) {
    llvm::APInt valueBits;
    if (!evaluateConstant(value, valueBits) || valueBits.getBitWidth() > 64)
        return false;
    result = valueBits.getZExtValue();
    return true;
}

AddressFamilies addressFamilies(const llvm::Module& module) {
    AddressFamilies families;
    const std::string triple = module.getTargetTriple().str();
    if (triple.find("windows") != std::string::npos ||
        triple.find("mingw") != std::string::npos)
        families.inet6 = 23;
    else if (triple.find("darwin") != std::string::npos ||
             triple.find("apple") != std::string::npos)
        families.inet6 = 30;
    return families;
}

CrossKind socketDomainKind(const llvm::CallBase& call,
                           const AddressFamilies& families, bool& confirmed) {
    confirmed = false;
    if (call.arg_size() == 0)
        return CrossKind::Remote;
    std::uint64_t domain = 0;
    if (!constantUInt(call.getArgOperand(0), domain))
        return CrossKind::Remote;
    if (domain == families.unixDomain) {
        confirmed = true;
        return CrossKind::Local;
    }
    if (domain == families.inet || domain == families.inet6) {
        confirmed = true;
        return CrossKind::Remote;
    }
    return CrossKind::Remote;
}

bool knownSocketKind(const llvm::Value* value,
                     const ClassificationContext& context, CrossKind& kind) {
    if (!value)
        return false;
    const auto found = context.socketKinds.find(value->stripPointerCasts());
    if (found == context.socketKinds.end())
        return false;
    kind = found->second;
    return true;
}

bool isLocalSocketHandle(const llvm::Value* value,
                         const ClassificationContext& context) {
    CrossKind kind = CrossKind::Remote;
    return knownSocketKind(value, context, kind) && kind == CrossKind::Local;
}

bool isRemoteSocketHandle(const llvm::Value* value,
                          const ClassificationContext& context) {
    CrossKind kind = CrossKind::Remote;
    return knownSocketKind(value, context, kind) && kind == CrossKind::Remote;
}

void appendUniqueCallee(std::vector<const llvm::Function*>& callees,
                        const llvm::Function* function) {
    if (function &&
        std::find(callees.begin(), callees.end(), function) == callees.end())
        callees.push_back(function);
}

IndirectCallees buildIndirectCallees(const llvm::Module& module) {
    IndirectCallees result;
    SVF::Options::PStat.setValue(false);
    SVF::LLVMModuleSet::buildSVFModule(const_cast<llvm::Module&>(module));
    SVF::SVFIRBuilder builder;
    SVF::SVFIR* pag = builder.build();
    auto* pta = new SVF::AndersenWaveDiff(
        pag, SVF::PTATY::AndersenWaveDiff_WPA, false);
    pta->disablePrintStat();
    pta->analyze();
    SVF::CallGraph* callGraph = pta->getCallGraph();
    SVF::LLVMModuleSet* moduleSet =
        SVF::LLVMModuleSet::getLLVMModuleSet();
    const auto& indirectMap = callGraph->getIndCallMap();

    auto appendByName = [&](std::vector<const llvm::Function*>& callees,
                            const std::string& name) {
        const llvm::Function* function = module.getFunction(name);
        if (!function)
            function = moduleSet->getFunction(name);
        appendUniqueCallee(callees, function);
    };
    auto collectFromPointsTo =
        [&](const SVF::PointsTo& pointsTo,
            std::vector<const llvm::Function*>& callees) {
            for (SVF::NodeID id : pointsTo) {
                const SVF::ObjVar* objectVariable = pag->getObjVar(id);
                if (!objectVariable)
                    continue;
                const SVF::BaseObjVar* object =
                    pag->getBaseObject(objectVariable->getId());
                if (object && object->isFunction()) {
                    const auto* function =
                        SVF::SVFUtil::cast<SVF::FunObjVar>(object)->getFunction();
                    if (function)
                        appendByName(callees, function->getName());
                }
            }
        };
    auto collectFromFields =
        [&](const SVF::PointsTo& pointsTo,
            std::vector<const llvm::Function*>& callees) {
            std::unordered_set<SVF::NodeID> visited;
            for (SVF::NodeID id : pointsTo) {
                const SVF::ObjVar* objectVariable = pag->getObjVar(id);
                const SVF::BaseObjVar* object =
                    objectVariable
                        ? pag->getBaseObject(objectVariable->getId())
                        : nullptr;
                if (!object)
                    continue;
                for (SVF::NodeID field :
                     pag->getAllFieldsObjVars(object)) {
                    if (visited.insert(field).second)
                        collectFromPointsTo(pta->getPts(field), callees);
                }
            }
        };

    for (const llvm::Function& function : module) {
        if (function.isDeclaration() || function.isIntrinsic())
            continue;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                const auto* call =
                    llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || call->getCalledFunction() ||
                    !moduleSet->hasICFGNode(call))
                    continue;
                const SVF::CallICFGNode* node =
                    moduleSet->getCallICFGNode(call);
                std::vector<const llvm::Function*> callees;
                const auto found = indirectMap.find(node);
                if (found != indirectMap.end()) {
                    for (const SVF::FunObjVar* target : found->second)
                        appendByName(callees, target->getName());
                }
                if (pag->isIndirectCallSites(node))
                    collectFromPointsTo(pta->getPts(pag->getFunPtr(node)),
                                        callees);
                const llvm::Value* pointer =
                    call->getCalledOperand()->stripPointerCasts();
                if (moduleSet->hasValueNode(pointer))
                    collectFromPointsTo(
                        pta->getPts(moduleSet->getValueNode(pointer)), callees);
                if (callees.empty()) {
                    if (const auto* load =
                            llvm::dyn_cast<llvm::LoadInst>(pointer)) {
                        const llvm::Value* address =
                            load->getPointerOperand()->stripPointerCasts();
                        for (const llvm::Value* root : {address, pointer}) {
                            if (!moduleSet->hasValueNode(root))
                                continue;
                            collectFromFields(
                                pta->getPts(moduleSet->getValueNode(root)),
                                callees);
                            if (!callees.empty())
                                break;
                        }
                    }
                }
                if (!callees.empty())
                    result[call] = std::move(callees);
            }
        }
    }

    delete pta;
    SVF::SVFIR::releaseSVFIR();
    SVF::LLVMModuleSet::releaseLLVMModuleSet();
    return result;
}

template <typename Map, typename Key>
bool insertKind(Map& map, const Key* value, CrossKind kind) {
    if (!value)
        return false;
    const auto found = map.find(value);
    if (found == map.end()) {
        map.emplace(value, kind);
        return true;
    }
    return false;
}

ClassificationContext buildSocketContext(const llvm::Module& module,
                                          const IndirectCallees& indirect) {
    ClassificationContext context;
    context.families = addressFamilies(module);
    bool changed;
    do {
        changed = false;
        for (const llvm::Function& function : module) {
            if (function.isDeclaration())
                continue;

            for (const llvm::BasicBlock& block : function) {
                for (const llvm::Instruction& instruction : block) {
                    if (const auto* call =
                            llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                        for (const llvm::Function* callee :
                             collectCallees(*call, indirect)) {
                            const std::string name = callee->getName().str();
                            CrossKind kind = CrossKind::Remote;
                            bool known = false;
                            if (name == "socket") {
                                bool confirmed = false;
                                kind = socketDomainKind(
                                    *call, context.families, confirmed);
                                known = confirmed;
                            } else if (name == "socketpair" ||
                                       name == "WSASocketA" ||
                                       name == "WSASocketW") {
                                kind = CrossKind::Local;
                                if (name == "socketpair")
                                    known = true;
                                else {
                                    bool confirmed = false;
                                    kind = socketDomainKind(
                                        *call, context.families, confirmed);
                                    known = confirmed;
                                }
                            } else if ((name == "dup" || name == "dup2" ||
                                        name == "dup3") &&
                                       call->arg_size() > 0) {
                                known = knownSocketKind(call->getArgOperand(0),
                                                        context, kind);
                            } else {
                                const auto found =
                                    context.functionReturnKinds.find(callee);
                                if (found !=
                                    context.functionReturnKinds.end()) {
                                    kind = found->second;
                                    known = true;
                                }
                            }
                            if (known &&
                                (instruction.getType()->isIntegerTy() ||
                                 instruction.getType()->isPointerTy()))
                                changed |= insertKind(context.socketKinds,
                                                      &instruction, kind);

                            if (name == "socketpair" && call->arg_size() > 3)
                                changed |= insertKind(
                                    context.socketKinds,
                                    llvm::getUnderlyingObject(
                                        call->getArgOperand(3)),
                                    CrossKind::Local);
                        }
                    }

                    if (const auto* store =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (knownSocketKind(store->getValueOperand(), context,
                                            kind))
                            changed |= insertKind(
                                context.socketKinds,
                                llvm::getUnderlyingObject(
                                    store->getPointerOperand()),
                                kind);
                    }
                    if (const auto* load =
                            llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (knownSocketKind(load->getPointerOperand(), context,
                                            kind))
                            changed |= insertKind(context.socketKinds, load,
                                                  kind);
                    }
                    if (const auto* phi =
                            llvm::dyn_cast<llvm::PHINode>(&instruction)) {
                        for (const llvm::Value* incoming :
                             phi->incoming_values()) {
                            CrossKind kind = CrossKind::Remote;
                            if (knownSocketKind(incoming, context, kind)) {
                                changed |= insertKind(context.socketKinds, phi,
                                                      kind);
                                break;
                            }
                        }
                    }
                    if (const auto* select =
                            llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (knownSocketKind(select->getTrueValue(), context,
                                            kind) ||
                            knownSocketKind(select->getFalseValue(), context,
                                            kind))
                            changed |= insertKind(context.socketKinds, select,
                                                  kind);
                    }
                    if (const auto* cast =
                            llvm::dyn_cast<llvm::CastInst>(&instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (knownSocketKind(cast->getOperand(0), context, kind))
                            changed |= insertKind(context.socketKinds, cast,
                                                  kind);
                    }
                    if (const auto* extract =
                            llvm::dyn_cast<llvm::ExtractValueInst>(
                                &instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (knownSocketKind(extract->getAggregateOperand(),
                                            context, kind))
                            changed |= insertKind(context.socketKinds, extract,
                                                  kind);
                    }
                    if (const auto* ret =
                            llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
                        CrossKind kind = CrossKind::Remote;
                        if (ret->getReturnValue() &&
                            knownSocketKind(ret->getReturnValue(), context,
                                            kind))
                            changed |= insertKind(context.functionReturnKinds,
                                                  &function, kind);
                    }
                }
            }
        }
    } while (changed);
    return context;
}

TriState mmapSharing(const llvm::CallBase& call) {
    if (call.arg_size() <= 3)
        return TriState::No;
    std::uint64_t flags = 0;
    if (!constantUInt(call.getArgOperand(3), flags))
        return TriState::Unknown;
    constexpr std::uint64_t kSharedMask = 0x3;
    const std::uint64_t kind = flags & kSharedMask;
    return kind == 0x1 || kind == 0x3 ? TriState::Yes : TriState::No;
}

TriState psharedState(const llvm::CallBase& call, unsigned index) {
    if (call.arg_size() <= index)
        return TriState::No;
    std::uint64_t value = 0;
    if (!constantUInt(call.getArgOperand(index), value))
        return TriState::Unknown;
    return value != 0 ? TriState::Yes : TriState::No;
}

bool fcntlLocksFile(const llvm::CallBase& call) {
    if (call.arg_size() <= 1)
        return false;
    std::uint64_t command = 0;
    if (!constantUInt(call.getArgOperand(1), command))
        return false;
    switch (command) {
    case 6:
    case 7:
    case 13:
    case 14:
    case 37:
    case 38:
        return true;
    default:
        return false;
    }
}

bool containsAny(const std::string& text,
                 const std::unordered_set<std::string>& patterns) {
    for (const std::string& pattern : patterns)
        if (text.find(pattern) != std::string::npos)
            return true;
    return false;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    std::size_t patternIndex = 0;
    std::size_t textIndex = 0;
    std::size_t wildcard = std::string::npos;
    std::size_t backtrack = 0;
    while (textIndex < text.size()) {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' ||
             pattern[patternIndex] == text[textIndex])) {
            ++patternIndex;
            ++textIndex;
        } else if (patternIndex < pattern.size() &&
                   pattern[patternIndex] == '*') {
            wildcard = patternIndex++;
            backtrack = textIndex;
        } else if (wildcard != std::string::npos) {
            patternIndex = wildcard + 1;
            textIndex = ++backtrack;
        } else {
            return false;
        }
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        ++patternIndex;
    return patternIndex == pattern.size();
}

bool configuredApiMatches(const std::unordered_set<std::string>& configured,
                          const std::string& name,
                          const std::string& demangled) {
    for (const std::string& pattern : configured) {
        if (pattern == name || pattern == demangled)
            return true;
        if (pattern.find('*') != std::string::npos ||
            pattern.find('?') != std::string::npos) {
            if (globMatch(pattern, name) || globMatch(pattern, demangled))
                return true;
        } else if (pattern.find("::") != std::string::npos &&
                   demangled.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct CallInfo {
    bool evidence = false;
    bool confirmed = true;
    bool returnsSharedHandle = false;
    CrossKind kind = CrossKind::Local;
    std::string mechanism;
};

CallInfo classifyCallWithCallee(const llvm::CallBase& call,
                                const llvm::Function& callee,
                                const ClassificationContext& context) {
    CallInfo info;
    const std::string name = callee.getName().str();
    const std::string demangled = llvm::demangle(name);

    static const std::unordered_set<std::string> kProcessApis = {
        "fork", "vfork", "clone", "clone3", "posix_spawn", "posix_spawnp",
        "system", "popen", "_popen", "execl", "execle", "execlp", "execv",
        "execve", "execveat", "execvp", "execvpe", "CreateProcessA",
        "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW",
        "CreateProcessWithLogonW", "WinExec", "ShellExecuteA",
        "ShellExecuteW", "ShellExecuteExA", "ShellExecuteExW"};
    static const std::unordered_set<std::string> kSharedMemoryApis = {
        "shmget", "shmat", "shmdt", "shmctl", "shm_open", "shm_unlink",
        "memfd_create", "MapViewOfFile",
        "MapViewOfFileEx", "MapViewOfFileNuma", "MapViewOfFileNuma2",
        "CreateFileMappingA", "CreateFileMappingW",
        "CreateFileMappingNumaA", "CreateFileMappingNumaW",
        "OpenFileMappingA", "OpenFileMappingW"};
    static const std::unordered_set<std::string> kLocalIpcApis = {
        "socketpair", "pipe", "pipe2", "mkfifo", "mkfifoat", "msgget",
        "msgsnd", "msgrcv", "msgctl", "semget", "semop", "semtimedop",
        "semctl", "mq_open", "mq_close", "mq_unlink", "mq_send",
        "mq_receive", "mq_timedsend", "mq_timedreceive", "mq_getattr",
        "mq_setattr", "mq_notify", "sem_open", "sem_close", "sem_unlink",
        "flock", "lockf", "eventfd", "eventfd2", "signalfd", "signalfd4",
        "userfaultfd", "pidfd_getfd", "process_madvise",
        "CreateNamedPipeA", "CreateNamedPipeW", "ConnectNamedPipe",
        "CreateMutexA", "CreateMutexW", "OpenMutexA", "OpenMutexW",
        "CreateSemaphoreA", "CreateSemaphoreW", "OpenSemaphoreA",
        "OpenSemaphoreW", "CreateEventA", "CreateEventW", "OpenEventA",
        "OpenEventW", "CreateRemoteThread", "NtCreateThreadEx",
        "QueueUserAPC", "SetWindowsHookExA", "SetWindowsHookExW",
        "xpc_connection_create", "xpc_connection_create_mach_service",
        "xpc_connection_send_message", "mach_msg", "bootstrap_look_up"};
    static const std::unordered_set<std::string> kRemoteApis = {
        "connect", "accept", "accept4", "send", "sendto", "sendmsg",
        "sendmmsg", "recvmmsg", "sendfile", "recv", "recvfrom", "recvmsg",
        "bind", "listen", "shutdown", "SSL_connect", "SSL_write",
        "SSL_read", "SSL_do_handshake", "WSAConnect", "WSASend",
        "WSARecv", "WSASendTo", "WSARecvFrom"};
    static const std::unordered_set<std::string> kNetworkIoApis = {
        "send", "sendto", "sendmsg", "sendmmsg", "recvmmsg", "sendfile",
        "recv", "recvfrom", "recvmsg", "SSL_write", "SSL_read",
        "WSASend", "WSARecv", "WSASendTo", "WSARecvFrom"};
    static const std::unordered_set<std::string> kProcessMemoryApis = {
        "process_vm_readv", "process_vm_writev", "ptrace",
        "ReadProcessMemory", "WriteProcessMemory", "NtReadVirtualMemory",
        "NtWriteVirtualMemory", "VirtualAllocEx", "VirtualProtectEx",
        "NtMapViewOfSection", "ZwMapViewOfSection", "task_for_pid",
        "mach_vm_read", "mach_vm_write"};
    static const std::unordered_set<std::string> kSharedSyncApis = {
        "pthread_mutexattr_setpshared", "pthread_condattr_setpshared",
        "pthread_rwlockattr_setpshared", "sem_init"};
    static const std::unordered_set<std::string> kHandleSourceApis = {
        "shmget", "shmat", "shm_open", "memfd_create", "MapViewOfFile",
        "MapViewOfFileEx", "MapViewOfFileNuma", "MapViewOfFileNuma2",
        "CreateFileMappingA", "CreateFileMappingW",
        "CreateFileMappingNumaA", "CreateFileMappingNumaW",
        "OpenFileMappingA", "OpenFileMappingW", "socket", "mq_open",
        "sem_open", "msgget", "semget", "CreateMutexA", "CreateMutexW",
        "OpenMutexA", "OpenMutexW", "CreateSemaphoreA", "CreateSemaphoreW",
        "OpenSemaphoreA", "OpenSemaphoreW", "CreateEventA", "CreateEventW",
        "OpenEventA", "OpenEventW"};
    static const std::unordered_set<std::string> kLocalPatterns = {
        "boost::interprocess::", "boost::asio::local::",
        "shared_memory_object", "managed_shared_memory",
        "managed_mapped_file", "windows_shared_memory",
        "anonymous_shared_memory", "mapped_region", "message_queue"};
    static const std::unordered_set<std::string> kRemotePatterns = {
        "boost::asio::ip::", "boost::beast::", "curl_", "libcurl"};

    info.kind = CrossKind::Local;
    if (name == "mmap" || name == "mmap64") {
        const TriState sharing = mmapSharing(call);
        if (sharing == TriState::No)
            return info;
        info.evidence = true;
        info.confirmed = sharing == TriState::Yes;
        info.returnsSharedHandle = true;
        info.mechanism = "shared memory mapping via " + name + "()";
        return info;
    }
    if (name == "socket" || name == "WSASocketA" || name == "WSASocketW") {
        bool confirmed = false;
        info.kind = socketDomainKind(call, context.families, confirmed);
        info.evidence = true;
        info.confirmed = confirmed;
        info.returnsSharedHandle = true;
        info.mechanism = info.kind == CrossKind::Local
                             ? "local IPC socket via " + name + "()"
                             : "network socket via " + name + "()";
        return info;
    }
    if (kRemoteApis.count(name) != 0) {
        if (call.arg_size() > 0 &&
            isLocalSocketHandle(call.getArgOperand(0), context))
            return info;
        info.evidence = true;
        info.kind = CrossKind::Remote;
        if (kNetworkIoApis.count(name) != 0)
            info.confirmed = call.arg_size() > 0 &&
                             isRemoteSocketHandle(call.getArgOperand(0),
                                                  context);
        else
            info.confirmed = call.arg_size() == 0 ||
                             isRemoteSocketHandle(call.getArgOperand(0),
                                                  context);
        info.mechanism = "network communication via " + name + "()";
        return info;
    }
    if (name == "sem_init" || kSharedSyncApis.count(name) != 0) {
        const TriState shared = psharedState(call, 1);
        if (shared == TriState::No)
            return info;
        info.evidence = true;
        info.confirmed = shared == TriState::Yes;
        info.mechanism = "process-shared synchronization via " + name + "()";
        return info;
    }
    if (name == "fcntl") {
        if (!fcntlLocksFile(call))
            return info;
        info.evidence = true;
        info.mechanism = "cross-process file lock via fcntl()";
        return info;
    }
    if (kProcessApis.count(name) != 0) {
        info.evidence = true;
        info.mechanism = "process creation/execution via " + name + "()";
        return info;
    }
    if (kSharedMemoryApis.count(name) != 0) {
        info.evidence = true;
        info.returnsSharedHandle = kHandleSourceApis.count(name) != 0;
        info.mechanism = "shared-memory API " + name + "()";
        return info;
    }
    if (kLocalIpcApis.count(name) != 0) {
        info.evidence = true;
        info.returnsSharedHandle = kHandleSourceApis.count(name) != 0;
        info.mechanism = "local IPC endpoint via " + name + "()";
        return info;
    }
    if (kProcessMemoryApis.count(name) != 0) {
        info.evidence = true;
        info.mechanism = "cross-process memory access via " + name + "()";
        return info;
    }
    if (containsAny(demangled, kLocalPatterns)) {
        info.evidence = true;
        info.mechanism = "local interprocess API " + demangled;
        return info;
    }
    if (containsAny(demangled, kRemotePatterns)) {
        info.evidence = true;
        info.kind = CrossKind::Remote;
        info.mechanism = "remote communication API " + demangled;
        return info;
    }
    if (configuredApiMatches(gLocalApis, name, demangled)) {
        info.evidence = true;
        info.kind = CrossKind::Local;
        info.mechanism = "configured local cross-process API " + name + "()";
        return info;
    }
    if (configuredApiMatches(gRemoteApis, name, demangled)) {
        info.evidence = true;
        info.kind = CrossKind::Remote;
        info.mechanism = "configured remote cross-process API " + name + "()";
        return info;
    }
    const auto returned = context.functionReturnKinds.find(&callee);
    if (returned != context.functionReturnKinds.end()) {
        info.returnsSharedHandle = true;
        info.kind = returned->second;
    }
    return info;
}

bool isHandleUseApi(const std::string& name) {
    static const std::unordered_set<std::string> kHandleUseApis = {
        "read", "write", "pread", "pwrite", "readv", "writev", "recv",
        "send", "ftruncate", "lseek", "sem_wait", "sem_trywait",
        "sem_timedwait", "sem_post", "pthread_mutex_lock",
        "pthread_mutex_unlock", "pthread_cond_wait", "pthread_cond_signal",
        "mq_send", "mq_receive", "mq_timedsend", "mq_timedreceive", "semop"};
    return kHandleUseApis.count(name) != 0;
}

struct CallbackBinding {
    unsigned callbackIndex = 0;
    unsigned userDataIndex = 0;
    unsigned callbackParamIndex = 0;
};

bool callbackBinding(const std::string& name, CallbackBinding& binding) {
    if (name == "pthread_create" || name == "CreateThread" ||
        name == "CreateRemoteThread") {
        binding = {2, 3, 0};
        return true;
    }
    if (name == "_beginthread" || name == "_beginthreadex") {
        binding = {name == "_beginthread" ? 0U : 2U, 1, 0};
        return true;
    }
    return false;
}

bool feasibleEdge(const llvm::BasicBlock& predecessor,
                  const llvm::BasicBlock& successor) {
    const auto* branch =
        llvm::dyn_cast<llvm::BranchInst>(predecessor.getTerminator());
    if (!branch || !branch->isConditional())
        return true;
    llvm::APInt conditionValue;
    if (!evaluateConstant(branch->getCondition(), conditionValue) ||
        conditionValue.getBitWidth() == 0)
        return true;
    const unsigned taken = conditionValue.isZero() ? 1 : 0;
    return branch->getSuccessor(taken) == &successor;
}

std::unordered_set<const llvm::BasicBlock*> reachableBlocks(
    const llvm::Function& function) {
    std::unordered_set<const llvm::BasicBlock*> reachable;
    std::vector<const llvm::BasicBlock*> pending{&function.getEntryBlock()};
    while (!pending.empty()) {
        const llvm::BasicBlock* block = pending.back();
        pending.pop_back();
        if (!reachable.insert(block).second)
            continue;
        for (const llvm::BasicBlock* successor : llvm::successors(block)) {
            if (feasibleEdge(*block, *successor))
                pending.push_back(successor);
        }
    }
    return reachable;
}

bool isSharedValue(const llvm::Value* value,
                   const std::set<const llvm::Value*>& shared,
                   const std::set<const llvm::GlobalVariable*>& globals) {
    if (!value)
        return false;
    value = value->stripPointerCasts();
    if (shared.count(value) != 0)
        return true;
    if (value->getType()->isPointerTy()) {
        const llvm::Value* object = llvm::getUnderlyingObject(value);
        if (const auto* global =
                llvm::dyn_cast<llvm::GlobalVariable>(object))
            return globals.count(global) != 0;
    }
    return false;
}

bool markSharedGlobal(const llvm::Value* pointer,
                      std::set<const llvm::GlobalVariable*>& globals) {
    if (!pointer || !pointer->getType()->isPointerTy())
        return false;
    const llvm::Value* object = llvm::getUnderlyingObject(pointer);
    const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(object);
    return global && globals.insert(global).second;
}

const llvm::AllocaInst* sharedSlot(const llvm::Value* pointer) {
    if (!pointer || !pointer->getType()->isPointerTy())
        return nullptr;
    return llvm::dyn_cast<llvm::AllocaInst>(
        llvm::getUnderlyingObject(pointer));
}

bool propagateDerivedValue(
    const llvm::Instruction& instruction,
    const std::set<const llvm::Value*>& shared,
    const std::set<const llvm::GlobalVariable*>& globals,
    std::set<const llvm::Value*>& result) {
    if (result.count(&instruction) != 0)
        return false;
    if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction))
        return isSharedValue(gep->getPointerOperand(), shared, globals) &&
               result.insert(&instruction).second;
    if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(&instruction))
        return isSharedValue(cast->getOperand(0), shared, globals) &&
               result.insert(&instruction).second;
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
        for (const llvm::Value* incoming : phi->incoming_values()) {
            if (isSharedValue(incoming, shared, globals))
                return result.insert(&instruction).second;
        }
        return false;
    }
    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
        if (isSharedValue(select->getTrueValue(), shared, globals) ||
            isSharedValue(select->getFalseValue(), shared, globals))
            return result.insert(&instruction).second;
    }
    if (const auto* extract =
            llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
        if (isSharedValue(extract->getAggregateOperand(), shared, globals))
            return result.insert(&instruction).second;
    }
    if (const auto* insert =
            llvm::dyn_cast<llvm::InsertValueInst>(&instruction)) {
        if (isSharedValue(insert->getInsertedValueOperand(), shared, globals) ||
            isSharedValue(insert->getAggregateOperand(), shared, globals))
            return result.insert(&instruction).second;
    }
    return false;
}

struct Evidence {
    const llvm::Function* origin = nullptr;
    std::string mechanism;
    bool confirmed = true;

    friend bool operator<(const Evidence& left, const Evidence& right) {
        if (left.origin != right.origin)
            return std::less<const llvm::Function*>()(left.origin,
                                                      right.origin);
        if (left.mechanism != right.mechanism)
            return left.mechanism < right.mechanism;
        return left.confirmed < right.confirmed;
    }
};

struct Summary {
    std::set<Evidence> evidence;
    std::unordered_map<const llvm::Instruction*, std::set<Evidence>>
        siteEvidence;
    std::set<unsigned> sharedParams;
};

using Summaries = std::unordered_map<const llvm::Function*, Summary>;

bool addEvidence(Summary& summary, const llvm::Function& origin,
                 const llvm::Instruction& site, const std::string& mechanism,
                 bool confirmed) {
    const Evidence evidence{&origin, mechanism, confirmed};
    bool changed = summary.evidence.insert(evidence).second;
    changed |= summary.siteEvidence[&site].insert(evidence).second;
    return changed;
}

std::vector<const llvm::Function*> collectCallees(
    const llvm::CallBase& call, const IndirectCallees& indirect) {
    std::vector<const llvm::Function*> callees;
    appendUniqueCallee(callees,
                       resolveDirectFunction(call.getCalledOperand()));
    const auto found = indirect.find(&call);
    if (found != indirect.end()) {
        for (const llvm::Function* candidate : found->second)
            appendUniqueCallee(callees, candidate);
    }
    return callees;
}

std::set<const llvm::Value*> computeSharedValues(
    const llvm::Function& function, const Summary& summary,
    const std::set<const llvm::GlobalVariable*>& globals,
    const IndirectCallees& indirect, const ClassificationContext& context,
    CrossKind kind) {
    const std::unordered_set<const llvm::BasicBlock*> reachable =
        reachableBlocks(function);
    std::set<const llvm::Value*> shared;
    for (const llvm::Argument& argument : function.args()) {
        if (summary.sharedParams.count(argument.getArgNo()) != 0)
            shared.insert(&argument);
    }

    for (const llvm::BasicBlock& block : function) {
        if (reachable.count(&block) == 0)
            continue;
        for (const llvm::Instruction& instruction : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                for (const llvm::Function* callee :
                     collectCallees(*call, indirect)) {
                    const CallInfo info =
                        classifyCallWithCallee(*call, *callee, context);
                    if (info.kind == kind && info.returnsSharedHandle &&
                        info.evidence &&
                        (instruction.getType()->isPointerTy() ||
                         instruction.getType()->isIntegerTy()))
                        shared.insert(&instruction);
                }
            }
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                const llvm::Value* object =
                    llvm::getUnderlyingObject(load->getPointerOperand());
                if (const auto* global =
                        llvm::dyn_cast<llvm::GlobalVariable>(object)) {
                    if (globals.count(global) != 0)
                        shared.insert(load);
                }
            }
        }
    }

    std::set<const llvm::AllocaInst*> slots;
    bool changed;
    do {
        changed = false;
        for (const llvm::BasicBlock& block : function) {
            if (reachable.count(&block) == 0)
                continue;
            for (const llvm::Instruction& instruction : block) {
                if (const auto* store =
                        llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (isSharedValue(store->getValueOperand(), shared,
                                      globals)) {
                        if (const llvm::AllocaInst* slot =
                                sharedSlot(store->getPointerOperand())) {
                            changed |= slots.insert(slot).second;
                            changed |= shared.insert(slot).second;
                        }
                    }
                }
                if (const auto* load =
                        llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    if (const llvm::AllocaInst* slot =
                            sharedSlot(load->getPointerOperand())) {
                        if (slots.count(slot) != 0)
                            changed |= shared.insert(load).second;
                    }
                }
                if (const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    for (const llvm::Function* callee :
                         collectCallees(*call, indirect)) {
                        const std::string name = callee->getName().str();
                        const llvm::Value* out = nullptr;
                        if (kind == CrossKind::Local &&
                            (name == "pipe" || name == "pipe2") &&
                            call->arg_size() > 0)
                            out = call->getArgOperand(0);
                        else if (kind == CrossKind::Local &&
                                 name == "socketpair" && call->arg_size() > 3)
                            out = call->getArgOperand(3);
                        if (const llvm::AllocaInst* slot = sharedSlot(out)) {
                            changed |= slots.insert(slot).second;
                            changed |= shared.insert(slot).second;
                        }
                    }
                }
                changed |= propagateDerivedValue(instruction, shared, globals,
                                                 shared);
            }
        }
    } while (changed);
    return shared;
}

bool sharedAccess(const llvm::Instruction& instruction,
                  const std::set<const llvm::Value*>& shared,
                  const std::set<const llvm::GlobalVariable*>& globals,
                  std::string& mechanism) {
    const llvm::Value* pointer = nullptr;
    bool writes = false;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        pointer = load->getPointerOperand();
    } else if (const auto* store =
                   llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        pointer = store->getPointerOperand();
        writes = true;
    } else if (const auto* rmw =
                   llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
        pointer = rmw->getPointerOperand();
        writes = true;
    } else if (const auto* cmpxchg =
                   llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
        pointer = cmpxchg->getPointerOperand();
        writes = true;
    }
    if (!isSharedValue(pointer, shared, globals))
        return false;
    const llvm::Value* object =
        pointer && pointer->getType()->isPointerTy()
            ? llvm::getUnderlyingObject(pointer)
            : nullptr;
    if ((llvm::isa<llvm::GlobalVariable>(object) ||
         llvm::isa<llvm::AllocaInst>(object)) &&
        !instruction.getType()->isPointerTy())
        return false;
    mechanism = std::string("cross-process shared-memory ") +
                (writes ? "write" : "read");
    return true;
}

Summaries buildSummaries(const llvm::Module& module,
                         const IndirectCallees& indirect,
                         const ClassificationContext& context,
                         CrossKind kind) {
    Summaries summaries;
    for (const llvm::Function& function : module) {
        if (!function.isDeclaration())
            summaries.emplace(&function, Summary{});
    }

    std::set<const llvm::GlobalVariable*> sharedGlobals;
    bool changed;
    do {
        changed = false;
        for (const llvm::Function& function : module) {
            if (function.isDeclaration())
                continue;
            Summary& summary = summaries.at(&function);
            const std::set<const llvm::Value*> shared = computeSharedValues(
                function, summary, sharedGlobals, indirect, context, kind);
            const std::unordered_set<const llvm::BasicBlock*> reachable =
                reachableBlocks(function);

            for (const llvm::BasicBlock& block : function) {
                if (reachable.count(&block) == 0)
                    continue;
                for (const llvm::Instruction& instruction : block) {
                    const auto* call =
                        llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call) {
                        const std::vector<const llvm::Function*> callees =
                            collectCallees(*call, indirect);
                        for (const llvm::Function* callee : callees) {
                            const CallInfo info = classifyCallWithCallee(
                                *call, *callee, context);
                            if (info.evidence && info.kind == kind) {
                                changed |= addEvidence(summary, function,
                                                       instruction,
                                                       info.mechanism,
                                                       info.confirmed);
                            }

                            const std::string name = callee->getName().str();
                            if (kind == CrossKind::Local &&
                                (name == "pipe" || name == "pipe2")) {
                                if (call->arg_size() > 0)
                                    changed |= markSharedGlobal(
                                        call->getArgOperand(0), sharedGlobals);
                            } else if (kind == CrossKind::Local &&
                                       name == "socketpair" &&
                                       call->arg_size() > 3) {
                                changed |= markSharedGlobal(
                                    call->getArgOperand(3), sharedGlobals);
                            }

                            const auto found = summaries.find(callee);
                            if (found != summaries.end()) {
                                Summary& calleeSummary = found->second;
                                for (unsigned i = 0; i < call->arg_size(); ++i) {
                                    if (isSharedValue(call->getArgOperand(i),
                                                      shared, sharedGlobals)) {
                                        changed |=
                                            calleeSummary.sharedParams.insert(i)
                                                .second;
                                    }
                                }
                                for (const Evidence& item :
                                     calleeSummary.evidence) {
                                    changed |=
                                        addEvidence(summary, *item.origin,
                                                    instruction,
                                                    item.mechanism,
                                                    item.confirmed);
                                }
                            }

                            if (!(info.evidence && info.kind == kind) &&
                                call->arg_size() > 0 &&
                                isHandleUseApi(name) &&
                                isSharedValue(call->getArgOperand(0), shared,
                                              sharedGlobals)) {
                                changed |= addEvidence(
                                    summary, function, instruction,
                                    std::string("use of ") +
                                        (kind == CrossKind::Local
                                             ? "local IPC"
                                             : "remote network") +
                                        " handle via " + name + "()",
                                    false);
                            }
                        }

                        const llvm::Function* directCallee =
                            resolveDirectFunction(call->getCalledOperand());
                        CallbackBinding binding;
                        if (directCallee &&
                            callbackBinding(directCallee->getName().str(),
                                            binding) &&
                            binding.callbackIndex < call->arg_size() &&
                            binding.userDataIndex < call->arg_size() &&
                            isSharedValue(
                                call->getArgOperand(binding.userDataIndex),
                                shared, sharedGlobals)) {
                            const llvm::Function* callback =
                                resolveDirectFunction(
                                    call->getArgOperand(binding.callbackIndex));
                            if (callback) {
                                const auto found = summaries.find(callback);
                                if (found != summaries.end())
                                    changed |= found->second.sharedParams
                                                   .insert(
                                                       binding.callbackParamIndex)
                                                   .second;
                            }
                        }

                        for (unsigned i = 0; i < call->arg_size(); ++i) {
                            const llvm::Function* callback =
                                resolveDirectFunction(call->getArgOperand(i));
                            if (!callback)
                                continue;
                            const auto found = summaries.find(callback);
                            if (found == summaries.end())
                                continue;
                            for (const Evidence& item :
                                 found->second.evidence) {
                                changed |= addEvidence(summary, *item.origin,
                                                       instruction,
                                                       item.mechanism,
                                                       item.confirmed);
                            }
                        }
                    }

                    std::string mechanism;
                    if (sharedAccess(instruction, shared, sharedGlobals,
                                     mechanism)) {
                        changed |= addEvidence(summary, function, instruction,
                                               mechanism, false);
                    }

                    if (const auto* store =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (isSharedValue(store->getValueOperand(), shared,
                                          sharedGlobals)) {
                            changed |= markSharedGlobal(
                                store->getPointerOperand(), sharedGlobals);
                        }
                    }
                }
            }
        }
    } while (changed);
    return summaries;
}

std::string describeEvidence(const llvm::Function& function,
                             const std::set<Evidence>& evidence) {
    std::set<std::string> descriptions;
    for (const Evidence& item : evidence) {
        const std::string prefix = item.confirmed ? "" : "possible ";
        if (item.origin == &function) {
            descriptions.insert(prefix + item.mechanism);
        } else {
            descriptions.insert(prefix + item.mechanism + " in " +
                                displayFunctionName(*item.origin) + "()");
        }
    }

    std::string result;
    for (const std::string& description : descriptions) {
        if (!result.empty())
            result += "; ";
        result += description;
    }
    return result;
}

bool hasAccessForKind(const llvm::Function& function,
                      const IndirectCallees& indirect,
                      const ClassificationContext& context, CrossKind kind) {
    const Summaries summaries =
        buildSummaries(*function.getParent(), indirect, context, kind);
    const auto found = summaries.find(&function);
    return found != summaries.end() && !found->second.evidence.empty();
}

void analyzeCrossProcessKind(const llvm::Module& module,
                             const std::string& input,
                             std::vector<Finding>& findings,
                             const IndirectCallees& indirect,
                             const ClassificationContext& context,
                             CrossKind kind, const char* rule,
                             const char* cwe, const char* severity,
                             const char* category, const char* label) {
    const Summaries summaries =
        buildSummaries(module, indirect, context, kind);
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;
        const auto found = summaries.find(&function);
        if (found == summaries.end() || found->second.siteEvidence.empty())
            continue;
        const std::unordered_set<const llvm::BasicBlock*> reachable =
            reachableBlocks(function);

        std::unordered_map<const llvm::Instruction*, unsigned> order;
        unsigned index = 0;
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block)
                order.emplace(&instruction, index++);
        }

        std::vector<std::pair<unsigned, const llvm::Instruction*>> sites;
        for (const auto& entry : found->second.siteEvidence) {
            const auto position = order.find(entry.first);
            if (position == order.end() ||
                reachable.count(entry.first->getParent()) == 0)
                continue;
            sites.emplace_back(position->second, entry.first);
        }
        std::sort(sites.begin(), sites.end(),
                  [](const std::pair<unsigned, const llvm::Instruction*>& left,
                     const std::pair<unsigned, const llvm::Instruction*>& right) {
                      return left.first < right.first;
                  });

        for (const auto& site : sites) {
            const auto evidence = found->second.siteEvidence.find(site.second);
            if (evidence == found->second.siteEvidence.end())
                continue;
            const bool allConfirmed = std::all_of(
                evidence->second.begin(), evidence->second.end(),
                [](const Evidence& item) { return item.confirmed; });
            addFinding(findings, input, function, *site.second, rule, cwe,
                       severity, category,
                       std::string("Function has ") + label + " access: " +
                           describeEvidence(function, evidence->second) + ".",
                       allConfirmed ? "confirmed" : "possible");
        }
    }
}

} // namespace

struct AnalysisContext {
    const llvm::Module* module = nullptr;
    IndirectCallees indirect;
    ClassificationContext classification;
};

namespace {

std::mutex gContextMutex;
std::shared_ptr<AnalysisContext> gContext;

std::shared_ptr<AnalysisContext> analysisContext(const llvm::Module& module) {
    std::lock_guard<std::mutex> guard(gContextMutex);
    if (gContext && gContext->module == &module)
        return gContext;
    auto context = std::make_shared<AnalysisContext>();
    context->module = &module;
    context->indirect = buildIndirectCallees(module);
    context->classification = buildSocketContext(module, context->indirect);
    gContext = context;
    return gContext;
}

void invalidateAnalysisContext() {
    std::lock_guard<std::mutex> guard(gContextMutex);
    gContext.reset();
}

} // namespace

void configureCrossProcessApis(const std::vector<std::string>& localApis,
                               const std::vector<std::string>& remoteApis) {
    {
        std::lock_guard<std::mutex> guard(gLocalApiMutex);
        gLocalApis.clear();
        gRemoteApis.clear();
        for (const std::string& api : localApis) {
            if (!api.empty())
                gLocalApis.insert(api);
        }
        for (const std::string& api : remoteApis) {
            if (!api.empty())
                gRemoteApis.insert(api);
        }
    }
    invalidateAnalysisContext();
}

void configureCrossProcessApis(const std::vector<std::string>& apis) {
    configureCrossProcessApis(apis, {});
}
bool hasLocalCrossProcessAccess(const llvm::Function& function) {
    const llvm::Module* module = function.getParent();
    if (!module || function.isDeclaration())
        return false;
    const auto context = analysisContext(*module);
    return hasAccessForKind(function, context->indirect,
                            context->classification, CrossKind::Local);
}

bool hasRemoteCrossProcessAccess(const llvm::Function& function) {
    const llvm::Module* module = function.getParent();
    if (!module || function.isDeclaration())
        return false;
    const auto context = analysisContext(*module);
    return hasAccessForKind(function, context->indirect,
                            context->classification, CrossKind::Remote);
}

bool hasCrossProcessAccess(const llvm::Function& function) {
    const llvm::Module* module = function.getParent();
    if (!module || function.isDeclaration())
        return false;
    const auto context = analysisContext(*module);
    return hasAccessForKind(function, context->indirect,
                            context->classification, CrossKind::Local) ||
           hasAccessForKind(function, context->indirect,
                            context->classification, CrossKind::Remote);
}

void analyzeLocalCrossProcess(const llvm::Module& module,
                              const std::string& input,
                              std::vector<Finding>& findings) {
    const auto context = analysisContext(module);
    analyzeCrossProcessKind(module, input, findings, context->indirect,
                            context->classification, CrossKind::Local,
                            "LOCAL_CROSS_PROCESS_ACCESS", "CWE-668", "info",
                            "cross-process-local", "local cross-process");
}

void analyzeRemoteCrossProcess(const llvm::Module& module,
                               const std::string& input,
                               std::vector<Finding>& findings) {
    const auto context = analysisContext(module);
    analyzeCrossProcessKind(module, input, findings, context->indirect,
                            context->classification, CrossKind::Remote,
                            "REMOTE_CROSS_PROCESS_ACCESS", "CWE-200", "medium",
                            "cross-process-remote", "remote cross-machine");
}

} // namespace buganalyzer
