#include "bug_analyzer_plugin.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace buganalyzer {
namespace {

bool getKnownObjectSize(const llvm::Value* pointer,
                        const llvm::Module& module, std::uint64_t& size) {
    return llvm::getObjectSize(pointer, size, module.getDataLayout(), nullptr);
}

bool getConstantUnsigned(const llvm::Value* value, std::uint64_t& number) {
    const auto* constant =
        llvm::dyn_cast<llvm::ConstantInt>(value->stripPointerCasts());
    if (!constant || constant->getValue().getActiveBits() > 64)
        return false;
    number = constant->getZExtValue();
    return true;
}

bool getMaximumUnsignedValue(const llvm::Value* value,
                             const llvm::CallBase& call,
                             llvm::LazyValueInfo& valueInfo,
                             const llvm::DominatorTree& dominatorTree,
                             std::uint64_t& number) {
    if (getConstantUnsigned(value, number))
        return true;
    if (!value->getType()->isIntegerTy())
        return false;
    const llvm::ConstantRange range = valueInfo.getConstantRange(
        const_cast<llvm::Value*>(value), const_cast<llvm::CallBase*>(&call),
        false);
    if (range.isEmptySet()) {
        number = 0;
        return true;
    }
    std::uint64_t maximumValue = 0;
    bool hasMaximum = false;
    if (!range.isFullSet()) {
        const llvm::APInt maximum = range.getUnsignedMax();
        if (maximum.getActiveBits() <= 64) {
            maximumValue = maximum.getZExtValue();
            hasMaximum = true;
        }
    }

    const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
    if (!load)
        return false;
    const auto* storage = llvm::dyn_cast<llvm::AllocaInst>(
        load->getPointerOperand()->stripPointerCasts());
    if (!storage) {
        if (hasMaximum)
            number = maximumValue;
        return hasMaximum;
    }

    const llvm::StoreInst* uniqueStore = nullptr;
    unsigned storeCount = 0;
    for (const llvm::BasicBlock& block : *call.getFunction()) {
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (store &&
                store->getPointerOperand()->stripPointerCasts() == storage) {
                ++storeCount;
                uniqueStore = store;
            }
        }
    }
    if (storeCount != 1) {
        if (hasMaximum)
            number = maximumValue;
        return hasMaximum;
    }

    for (const llvm::BasicBlock& block : *call.getFunction()) {
        const auto* branch =
            llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
        if (!branch || !branch->isConditional())
            continue;
        const auto* compare =
            llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
        if (!compare)
            continue;

        const llvm::Value* comparedValue = compare->getOperand(0);
        const llvm::Value* constantValue = compare->getOperand(1);
        llvm::CmpInst::Predicate predicate = compare->getPredicate();
        const auto* comparedLoad =
            llvm::dyn_cast<llvm::LoadInst>(comparedValue);
        if (!comparedLoad ||
            comparedLoad->getPointerOperand()->stripPointerCasts() != storage) {
            comparedValue = compare->getOperand(1);
            constantValue = compare->getOperand(0);
            predicate = llvm::CmpInst::getSwappedPredicate(predicate);
            comparedLoad = llvm::dyn_cast<llvm::LoadInst>(comparedValue);
        }
        if (!comparedLoad ||
            comparedLoad->getPointerOperand()->stripPointerCasts() != storage)
            continue;
        if (!dominatorTree.dominates(uniqueStore, comparedLoad))
            continue;

        const auto* constant =
            llvm::dyn_cast<llvm::ConstantInt>(constantValue);
        if (!constant || constant->getValue().getActiveBits() > 64)
            continue;
        for (unsigned successorIndex = 0; successorIndex != 2;
             ++successorIndex) {
            const llvm::BasicBlock* successor =
                branch->getSuccessor(successorIndex);
            const llvm::BasicBlockEdge edge(&block, successor);
            if (!dominatorTree.dominates(edge, call.getParent()))
                continue;
            const llvm::CmpInst::Predicate pathPredicate =
                successorIndex == 0
                    ? predicate
                    : llvm::CmpInst::getInversePredicate(predicate);
            const std::uint64_t bound = constant->getZExtValue();
            switch (pathPredicate) {
            case llvm::CmpInst::ICMP_ULE:
            case llvm::CmpInst::ICMP_EQ:
                if (!hasMaximum || bound < maximumValue)
                    maximumValue = bound;
                hasMaximum = true;
                break;
            case llvm::CmpInst::ICMP_ULT:
                if (!hasMaximum || (bound == 0 ? 0 : bound - 1) <
                                       maximumValue)
                    maximumValue = bound == 0 ? 0 : bound - 1;
                hasMaximum = true;
                break;
            default:
                break;
            }
        }
    }
    if (hasMaximum)
        number = maximumValue;
    return hasMaximum;
}

bool getWcharSize(const llvm::Module& module, std::uint64_t& size) {
    const llvm::Triple triple(module.getTargetTriple());
    if (triple.getArch() == llvm::Triple::UnknownArch)
        return false;
    size = triple.isOSWindows() ? 2 : 4;
    return true;
}

bool getConstantStringAtCall(const llvm::Value* value,
                            const llvm::CallBase& call,
                            const llvm::DominatorTree& dominatorTree,
                            llvm::StringRef& string) {
    if (llvm::getConstantStringInfo(value, string))
        return true;
    const auto* load =
        llvm::dyn_cast<llvm::LoadInst>(value->stripPointerCasts());
    if (!load)
        return false;
    const auto* storage = llvm::dyn_cast<llvm::AllocaInst>(
        load->getPointerOperand()->stripPointerCasts());
    if (!storage)
        return false;

    const llvm::StoreInst* sourceStore = nullptr;
    for (const llvm::BasicBlock& block : *call.getFunction()) {
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (!store ||
                store->getPointerOperand()->stripPointerCasts() != storage)
                continue;
            if (sourceStore)
                return false;
            sourceStore = store;
        }
    }
    return sourceStore && dominatorTree.dominates(sourceStore, load) &&
           llvm::getConstantStringInfo(sourceStore->getValueOperand(), string);
}

bool hasUnsafeScanConversion(llvm::StringRef format,
                             const llvm::CallBase& call,
                             unsigned formatIndex,
                             const llvm::Module& module,
                             bool variadicArgumentsAvailable) {
    std::uint64_t nextArgument = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%')
            continue;

        std::size_t j = i + 1;
        std::uint64_t positionalArgument = 0;
        bool hasPositionalArgument = false;
        if (j < format.size() &&
            std::isdigit(static_cast<unsigned char>(format[j]))) {
            const std::size_t start = j;
            while (j < format.size() &&
                   std::isdigit(static_cast<unsigned char>(format[j])))
                ++j;
            if (j < format.size() && format[j] == '$') {
                for (std::size_t digit = start; digit < j; ++digit) {
                    if (positionalArgument > (UINT64_MAX - 9) / 10) {
                        positionalArgument = UINT64_MAX;
                        break;
                    }
                    positionalArgument =
                        positionalArgument * 10 + (format[digit] - '0');
                }
                hasPositionalArgument = positionalArgument != 0;
                j += 1;
            } else {
                j = start;
            }
        }

        if (j < format.size() && format[j] == '%') {
            i = j;
            continue;
        }

        bool suppressed = false;
        bool hasWidth = false;
        bool allocates = false;
        bool wide = false;
        std::uint64_t width = 0;
        for (; j < format.size(); ++j) {
            const char ch = format[j];
            if (ch == '*') {
                suppressed = true;
            } else if (std::isdigit(static_cast<unsigned char>(ch))) {
                hasWidth = true;
                if (width <= (UINT64_MAX - 9) / 10)
                    width = width * 10 + (ch - '0');
                else
                    width = UINT64_MAX;
            } else if (ch == 'm') {
                allocates = true;
            } else if (ch == 'l') {
                wide = true;
            } else if (ch == '$' || ch == 'h' || ch == 'L' ||
                       ch == 'j' || ch == 'z' || ch == 't' || ch == 'q') {
            } else {
                break;
            }
        }
        if (j >= format.size())
            break;

        const char conversion = format[j];
        const bool stringConversion =
            conversion == 's' || conversion == '[' || conversion == 'c';
        bool unsafe = false;
        if (stringConversion && !suppressed && !allocates) {
            if ((conversion == 's' || conversion == '[') && !hasWidth)
                unsafe = true;

            if (variadicArgumentsAvailable) {
                if (hasPositionalArgument &&
                    positionalArgument == UINT64_MAX)
                    return true;
                const std::uint64_t argument =
                    hasPositionalArgument ? positionalArgument - 1
                                          : nextArgument;
                const std::uint64_t callArgument =
                    static_cast<std::uint64_t>(formatIndex) + 1 + argument;
                if (callArgument >= call.arg_size()) {
                    unsafe = true;
                } else {
                    std::uint64_t capacity = 0;
                    if (!getKnownObjectSize(
                            call.getArgOperand(callArgument), module,
                            capacity)) {
                        unsafe = true;
                    } else {
                        std::uint64_t required =
                            conversion == 'c'
                                ? (hasWidth ? width : 1)
                                : (hasWidth && width < UINT64_MAX
                                       ? width + 1
                                       : UINT64_MAX);
                        if (wide) {
                            std::uint64_t wcharSize = 0;
                            if (!getWcharSize(module, wcharSize) ||
                                required > UINT64_MAX / wcharSize) {
                                unsafe = true;
                            } else {
                                required *= wcharSize;
                            }
                        }
                        if (required > capacity)
                            unsafe = true;
                    }
                }
            } else {
                unsafe = true;
            }
        }
        if (unsafe)
            return true;

        if (!suppressed)
            ++nextArgument;
        if (conversion == '[') {
            std::size_t scansetEnd = j + 1;
            if (scansetEnd < format.size() && format[scansetEnd] == '^')
                ++scansetEnd;
            if (scansetEnd < format.size() && format[scansetEnd] == ']')
                ++scansetEnd;
            while (scansetEnd < format.size() &&
                   format[scansetEnd] != ']')
                ++scansetEnd;
            i = scansetEnd;
        }
    }
    return false;
}

bool hasSafeConstantSource(const llvm::CallBase& call,
                           const llvm::Module& module,
                           const llvm::DominatorTree& dominatorTree) {
    if (call.arg_size() < 2)
        return false;
    llvm::StringRef source;
    if (!getConstantStringAtCall(call.getArgOperand(1), call, dominatorTree,
                                 source))
        return false;
    std::uint64_t capacity = 0;
    return getKnownObjectSize(call.getArgOperand(0), module, capacity) &&
           source.size() < capacity;
}

bool hasPotentialCopyOverflow(const llvm::CallBase& call,
                              const llvm::Module& module,
                              llvm::LazyValueInfo& valueInfo,
                              const llvm::DominatorTree& dominatorTree,
                              unsigned destinationIndex,
                              unsigned lengthIndex) {
    if (call.arg_size() <= std::max(destinationIndex, lengthIndex))
        return false;
    std::uint64_t capacity = 0;
    std::uint64_t length = 0;
    if (!getKnownObjectSize(call.getArgOperand(destinationIndex), module,
                            capacity))
        return false;
    return !getMaximumUnsignedValue(call.getArgOperand(lengthIndex), call,
                                    valueInfo, dominatorTree, length) ||
           length > capacity;
}

bool hasProvenShortLiteralOutput(const llvm::CallBase& call,
                                const llvm::Module& module,
                                const llvm::DominatorTree& dominatorTree,
                                unsigned formatIndex) {
    if (call.arg_size() <= formatIndex)
        return false;
    std::uint64_t capacity = 0;
    if (!getKnownObjectSize(call.getArgOperand(0), module, capacity))
        return false;
    llvm::StringRef format;
    if (!getConstantStringAtCall(call.getArgOperand(formatIndex), call,
                                 dominatorTree, format))
        return false;

    std::uint64_t outputLength = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '%') {
            if (i + 1 >= format.size() || format[i + 1] != '%')
                return false;
            ++i;
        }
        if (outputLength == UINT64_MAX)
            return false;
        ++outputLength;
    }
    return outputLength < capacity;
}

bool hasKnownFreadOverflow(const llvm::CallBase& call,
                           const llvm::Module& module,
                           llvm::LazyValueInfo& valueInfo,
                           const llvm::DominatorTree& dominatorTree) {
    if (call.arg_size() < 3)
        return false;
    std::uint64_t capacity = 0;
    std::uint64_t elementSize = 0;
    std::uint64_t elementCount = 0;
    if (!getKnownObjectSize(call.getArgOperand(0), module, capacity))
        return false;
    const bool knownElementSize = getMaximumUnsignedValue(
        call.getArgOperand(1), call, valueInfo, dominatorTree, elementSize);
    const bool knownElementCount = getMaximumUnsignedValue(
        call.getArgOperand(2), call, valueInfo, dominatorTree, elementCount);
    if ((knownElementSize && elementSize == 0) ||
        (knownElementCount && elementCount == 0))
        return false;
    if (!knownElementSize || !knownElementCount)
        return true;
    if (elementCount != 0 && elementSize > UINT64_MAX / elementCount)
        return true;
    return elementSize * elementCount > capacity;
}

bool hasSafeBoundedStringCopy(const llvm::CallBase& call,
                              const llvm::Module& module,
                              llvm::LazyValueInfo& valueInfo,
                              const llvm::DominatorTree& dominatorTree,
                              unsigned countIndex, bool wide) {
    if (call.arg_size() <= countIndex)
        return false;
    std::uint64_t capacity = 0;
    std::uint64_t maxCount = 0;
    if (!getKnownObjectSize(call.getArgOperand(0), module, capacity) ||
        !getMaximumUnsignedValue(call.getArgOperand(countIndex), call,
                                 valueInfo, dominatorTree, maxCount))
        return false;
    if (wide) {
        std::uint64_t wcharSize = 0;
        if (!getWcharSize(module, wcharSize))
            return false;
        capacity /= wcharSize;
    }
    return maxCount <= capacity;
}

bool hasNameOrIntrinsicPrefix(const std::string& name,
                              llvm::StringRef plainName,
                              llvm::StringRef intrinsicPrefix) {
    return name == plainName ||
           llvm::StringRef(name).starts_with(intrinsicPrefix);
}

llvm::StringRef normalizedScanName(const std::string& name) {
    llvm::StringRef result(name);
    if (result.consume_front("__isoc23_") ||
        result.consume_front("__isoc99_"))
        return result;
    return name;
}

bool isScanFunction(llvm::StringRef name, unsigned& formatIndex) {
    if (name == "scanf" || name == "vscanf") {
        formatIndex = 0;
        return true;
    }
    if (name == "fscanf" || name == "sscanf" || name == "vfscanf" ||
        name == "vsscanf") {
        formatIndex = 1;
        return true;
    }
    return false;
}

bool isOutputFormatFunction(const std::string& name,
                            unsigned& formatIndex) {
    if (name == "printf" || name == "vprintf") {
        formatIndex = 0;
        return true;
    }
    if (name == "fprintf" || name == "vfprintf" || name == "sprintf" ||
        name == "vsprintf" || name == "dprintf" || name == "vdprintf" ||
        name == "syslog" || name == "vsyslog" || name == "asprintf" ||
        name == "vasprintf" || name == "__printf_chk") {
        formatIndex = 1;
        return true;
    }
    if (name == "__fprintf_chk") {
        formatIndex = 2;
        return true;
    }
    if (name == "snprintf" || name == "vsnprintf") {
        formatIndex = 2;
        return true;
    }
    if (name == "__sprintf_chk") {
        formatIndex = 3;
        return true;
    }
    if (name == "__snprintf_chk") {
        formatIndex = 4;
        return true;
    }
    return false;
}

const llvm::Function* getDirectCallee(const llvm::Value* calledOperand) {
    while (calledOperand) {
        calledOperand = calledOperand->stripPointerCasts();
        if (const auto* function =
                llvm::dyn_cast<llvm::Function>(calledOperand))
            return function;
        const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(calledOperand);
        if (!alias)
            return nullptr;
        calledOperand = alias->getAliasee();
    }
    return nullptr;
}

struct FunctionPointerTarget {
    const llvm::Function* function = nullptr;
    bool ambiguous = false;
};

using FunctionPointerTargets =
    std::unordered_map<const llvm::AllocaInst*, FunctionPointerTarget>;

FunctionPointerTargets getFunctionPointerTargets(
    const llvm::Function& caller) {
    FunctionPointerTargets targets;
    for (const llvm::BasicBlock& block : caller) {
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (!store)
                continue;
            const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(
                store->getPointerOperand()->stripPointerCasts());
            if (!alloca)
                continue;

            FunctionPointerTarget& target = targets[alloca];
            const llvm::Function* storedFunction =
                getDirectCallee(store->getValueOperand());
            if (!storedFunction ||
                (target.function && target.function != storedFunction)) {
                target.ambiguous = true;
            } else {
                target.function = storedFunction;
            }
        }
    }
    return targets;
}

const llvm::Function* getCallee(
    const llvm::CallBase& call,
    const FunctionPointerTargets& functionPointerTargets) {
    if (const llvm::Function* callee =
            getDirectCallee(call.getCalledOperand()))
        return callee;

    const auto* load = llvm::dyn_cast<llvm::LoadInst>(
        call.getCalledOperand()->stripPointerCasts());
    if (!load)
        return nullptr;
    const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(
        load->getPointerOperand()->stripPointerCasts());
    if (!alloca)
        return nullptr;
    const auto target = functionPointerTargets.find(alloca);
    if (target == functionPointerTargets.end() || target->second.ambiguous ||
        !target->second.function)
        return nullptr;
    bool hasPrecedingStore = false;
    for (const llvm::Instruction& instruction : *call.getParent()) {
        if (&instruction == &call)
            break;
        const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store &&
            store->getPointerOperand()->stripPointerCasts() == alloca)
            hasPrecedingStore = true;
    }
    return hasPrecedingStore ? target->second.function : nullptr;
}

void analyzeCall(const llvm::Module& module, const std::string& input,
                 const llvm::Function& function, const llvm::CallBase& call,
                 const FunctionPointerTargets& functionPointerTargets,
                 llvm::LazyValueInfo& valueInfo,
                 const llvm::DominatorTree& dominatorTree,
                 std::vector<Finding>& findings) {
    const auto* callee = getCallee(call, functionPointerTargets);
    if (!callee)
        return;

    const std::string name = callee->getName().str();
    if (name == "gets" || name == "getwd") {
        addFinding(findings, input, function, call, "UNBOUNDED_INPUT",
                   name == "gets" ? "CWE-242" : "CWE-120", "high",
                   "unsafe-api",
                   name == "gets"
                       ? "gets reads input without a destination size bound."
                       : "getwd writes the current directory to a caller "
                         "buffer without receiving its capacity.");
    } else if (name == "strncpy" || name == "wcsncpy") {
        if (!hasSafeBoundedStringCopy(call, module, valueInfo, dominatorTree, 2,
                                      name == "wcsncpy")) {
            addFinding(findings, input, function, call, "UNBOUNDED_COPY",
                       "CWE-120", "medium", "unsafe-api",
                       name + " may receive a count larger than the "
                              "destination buffer can hold.");
        }
    } else if (name == "strcpy" || name == "strcat" || name == "wcscpy" ||
               name == "wcscat" || name == "strncat" ||
               name == "wcsncat" || name == "sprintf" ||
               name == "vsprintf") {
        const bool safeLiteralCopy =
            name == "strcpy" &&
            hasSafeConstantSource(call, module, dominatorTree);
        if (!safeLiteralCopy) {
            addFinding(findings, input, function, call, "UNBOUNDED_COPY",
                       "CWE-120", "medium", "unsafe-api",
                       name + " does not enforce the destination buffer size; "
                              "review the buffer capacity and input bounds.");
        }
    } else if (name == "mktemp" || name == "tmpnam" ||
               name == "tempnam") {
        addFinding(findings, input, function, call, "INSECURE_TEMP_FILE",
                   "CWE-377", "medium", "unsafe-api",
                   name + " can create a predictable temporary path; use "
                          "an API that atomically creates the file, such "
                          "as mkstemp.");
    } else if (name == "system" || name == "popen") {
        llvm::StringRef command;
        if (call.arg_size() == 0 ||
            !getConstantStringAtCall(call.getArgOperand(0), call,
                                     dominatorTree, command)) {
            addFinding(findings, input, function, call, "SHELL_COMMAND",
                       "CWE-78", "medium", "command-execution",
                       name + " invokes a shell with a non-constant command; "
                              "verify that all command data is trusted or "
                              "properly constrained.");
        }
    } else if ((hasNameOrIntrinsicPrefix(name, "memset", "llvm.memset.") ||
                hasNameOrIntrinsicPrefix(name, "memcpy", "llvm.memcpy.") ||
                hasNameOrIntrinsicPrefix(name, "memmove",
                                         "llvm.memmove.") ||
                name == "memccpy") &&
               hasPotentialCopyOverflow(call, module, valueInfo,
                                        dominatorTree, 0,
                                        name == "memccpy" ? 3 : 2)) {
        addFinding(findings, input, function, call, "BUFFER_OVERFLOW",
                   "CWE-787", "high", "unsafe-api",
                   name + " may write more bytes than the known destination "
                          "object can hold.");
    } else if ((name == "read" || name == "recv" || name == "pread" ||
                name == "pread64" || name == "recvfrom" ||
                name == "readlink") &&
               hasPotentialCopyOverflow(call, module, valueInfo,
                                        dominatorTree, 1, 2)) {
        addFinding(findings, input, function, call, "BUFFER_OVERFLOW",
                   "CWE-787", "high", "unsafe-api",
                   name + " may write more bytes than the known destination "
                          "object can hold.");
    } else if ((name == "fgets" || name == "getcwd") &&
               hasPotentialCopyOverflow(call, module, valueInfo,
                                        dominatorTree, 0, 1)) {
        addFinding(findings, input, function, call, "BUFFER_OVERFLOW",
                   "CWE-787", "high", "unsafe-api",
                   name + " may write more bytes than the known destination "
                          "object can hold.");
    } else if (name == "readlinkat" &&
               hasPotentialCopyOverflow(call, module, valueInfo,
                                        dominatorTree, 2, 3)) {
        addFinding(findings, input, function, call, "BUFFER_OVERFLOW",
                   "CWE-787", "high", "unsafe-api",
                   name + " may write more bytes than the known destination "
                          "object can hold.");
    } else if (name == "fread" &&
               hasKnownFreadOverflow(call, module, valueInfo,
                                     dominatorTree)) {
        addFinding(findings, input, function, call, "BUFFER_OVERFLOW",
                   "CWE-787", "high", "unsafe-api",
                   "fread may write more bytes than the known destination "
                   "object can hold.");
    } else if ((name == "snprintf" || name == "vsnprintf") &&
               hasPotentialCopyOverflow(call, module, valueInfo,
                                        dominatorTree, 0, 1) &&
               !hasProvenShortLiteralOutput(call, module, dominatorTree, 2)) {
        addFinding(findings, input, function, call,
                   "FORMAT_SIZE_EXCEEDS_BUFFER", "CWE-787", "high",
                   "unsafe-api",
                   name + " may be given a size larger than the known "
                          "destination object.");
    }

    const llvm::StringRef scanName = normalizedScanName(name);
    unsigned formatIndex = 0;
    if (isScanFunction(scanName, formatIndex) &&
        call.arg_size() > formatIndex) {
        llvm::StringRef format;
        if (!getConstantStringAtCall(call.getArgOperand(formatIndex), call,
                                     dominatorTree, format) ||
            hasUnsafeScanConversion(format, call, formatIndex, module,
                                    !scanName.starts_with("v"))) {
            addFinding(findings, input, function, call, "UNBOUNDED_SCAN",
                       "CWE-120", "medium", "unsafe-api",
                       name + " uses a string/character conversion without "
                              "a proven destination bound that fits its "
                              "field width.");
        }
    }

    if (isOutputFormatFunction(name, formatIndex) &&
        call.arg_size() > formatIndex) {
        llvm::StringRef format;
        if (!getConstantStringAtCall(call.getArgOperand(formatIndex), call,
                                     dominatorTree, format)) {
            addFinding(findings, input, function, call,
                       "UNTRUSTED_FORMAT_STRING", "CWE-134", "high",
                       "unsafe-api",
                       name + " receives a non-constant format string; "
                              "untrusted format directives may expose memory "
                              "or corrupt program state.");
        }
    }
}

} // namespace

void analyzeUnsafeApis(const llvm::Module& module, const std::string& input,
                       std::vector<Finding>& findings) {
    for (const llvm::Function& function : module) {
        if (function.isDeclaration())
            continue;
        llvm::DominatorTree dominatorTree(
            const_cast<llvm::Function&>(function));
        llvm::AssumptionCache assumptions(
            const_cast<llvm::Function&>(function));
        llvm::LazyValueInfo valueInfo(&assumptions, &module.getDataLayout());
        const FunctionPointerTargets functionPointerTargets =
            getFunctionPointerTargets(function);
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    analyzeCall(module, input, function, *call,
                                functionPointerTargets, valueInfo,
                                dominatorTree, findings);
            }
        }
    }
}

} // namespace buganalyzer
