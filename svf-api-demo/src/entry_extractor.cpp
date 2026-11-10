//===----------------------------------------------------------------------===//
// entry_extractor.cpp — list the entry functions of an LLVM bitcode module.
//
// Usage: entry_extractor [options] <bitcode-file>
//
// "Entry function" has several readings; --mode selects one:
//   roots (default)  Functions that can be triggered from outside the module's
//                    own direct call graph:
//                      * main / svf.main;
//                      * framework-registered handlers (see below);
//                      * functions with no direct caller in this module:
//                        uncalled functions ("no-caller") and functions whose
//                        address escapes ("callback").
//                    Recursion and mutual recursion are handled with SCCs.
//   routes           Only the framework-registered handlers.
//   exported         Every function with a body that is visible to other
//                    modules (non-local linkage): the module's public API.
//   all              Every function with a body (inspection / debugging).
//
// A "framework-registered handler" is recognised in four ways:
//   route-handler       A constant dispatch table pairing a label with a
//                       function pointer. Labels may be C strings (including
//                       merged string pools / GEPs into them), string views or
//                       integral keys (HTTP methods, opcodes, message ids).
//                       The pointer may be plain, bitcast, a C++
//                       pointer-to-member (ptrtoint), a GlobalAlias, or live
//                       in another global constant referenced by the table.
//   registered-handler A function pointer stored or passed to a registration
//                       API at run time (router.add, std::function, subscribe,
//                       pthread_create, a global constructor's init call, ...).
//   annotated          A function listed in @llvm.global.annotations.
//   retained           A function listed in @llvm.used / @llvm.compiler.used.
//
// Still out of reach at the IR level (the analyzer's SVF pointer analysis is
// needed): type-erased completion handlers whose callable never appears as a
// function pointer (a lambda/std::function closure), and virtual
// pointer-to-member functions encoded as vtable indices rather than addresses.
//
// --verbose appends a tab-separated reason and, when available, the trigger
// (route label, registration call, annotation) that made the function an
// entry. Without --verbose one name per line is printed, in the module's
// mangled spelling (or demangled with --demangle); the output can be fed
// directly to `clang_analyzer --entry-file <file>`. Declarations without a
// body are never printed: they are library calls, not entry points defined in
// this bitcode.
//===----------------------------------------------------------------------===//

#include "function_filter.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Use.h>
#include <llvm/IR/User.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum class Mode { Roots, Routes, Exported, All };

const char* modeName(Mode mode) {
    switch (mode) {
    case Mode::Roots: return "roots";
    case Mode::Routes: return "routes";
    case Mode::Exported: return "exported";
    case Mode::All: return "all";
    }
    return "roots";
}

struct Options {
    Mode mode = Mode::Roots;
    bool demangle = false;
    bool verbose = false;
    bool includeLibrary = false;
    std::string outputFile;
    std::string bcFile;
};

void printUsage(const char* prog) {
    llvm::errs()
        << "Usage: " << prog << " [options] <bitcode-file>\n"
        << "Options:\n"
        << "  --mode <roots|routes|exported|all>\n"
        << "                     Which functions count as entries:\n"
        << "                       roots    - main, framework-registered\n"
        << "                                  handlers and functions with no\n"
        << "                                  direct caller [default]\n"
        << "                       routes   - only framework-registered\n"
        << "                                  handlers (route tables, runtime\n"
        << "                                  registration, annotations)\n"
        << "                       exported- every defined function visible\n"
        << "                                  to other modules\n"
        << "                       all      - every function with a body\n"
        << "  --include-library  Keep every address-taken callback in roots\n"
        << "                     mode, including library/framework ones\n"
        << "  --demangle         Print demangled C++ names\n"
        << "  --verbose          Append reason and trigger (tab-separated)\n"
        << "                     to each name and print a summary on stderr\n"
        << "  --output <file>    Write names to <file> instead of stdout\n"
        << "  --help             Show this help\n";
}

//===----------------------------------------------------------------------===//
// Shared helpers: labels, function references and handler bookkeeping.
//===----------------------------------------------------------------------===//

/// Why a function was recognised as a framework entry. Ordered by confidence:
/// a route table beats a runtime registration, which beats a metadata hint.
enum class HandlerKind { RouteTable = 0, Registration = 1, Annotation = 2,
                         Retained = 3 };

const char* kindReason(HandlerKind kind) {
    switch (kind) {
    case HandlerKind::RouteTable: return "route-handler";
    case HandlerKind::Registration: return "registered-handler";
    case HandlerKind::Annotation: return "annotated";
    case HandlerKind::Retained: return "retained";
    }
    return "route-handler";
}

struct HandlerRecord {
    HandlerKind kind;
    std::string trigger;
};

using HandlerMap = std::unordered_map<const llvm::Function*, HandlerRecord>;

std::string sanitize(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    if (s.size() > 160)
        s.resize(160);
    return s;
}

/// I8 bytes of a string constant. The initializer may be a ConstantDataArray
/// directly or behind a global (the usual `@.str` form), and does not have to
/// be NUL-terminated: a std::string_view / string pool is a valid label too.
std::string constantBytes(const llvm::Constant* C) {
    if (!C)
        return {};
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
        if (!GV->hasInitializer())
            return {};
        C = GV->getInitializer();
    }
    const llvm::Value* stripped = C->stripPointerCasts();
    const auto* data = llvm::dyn_cast<llvm::ConstantDataArray>(stripped);
    if (!data || !data->getElementType()->isIntegerTy(8))
        return {};
    std::string bytes;
    bytes.reserve(data->getNumElements());
    for (unsigned i = 0; i < data->getNumElements(); ++i)
        bytes.push_back(static_cast<char>(data->getElementAsInteger(i) & 0xff));
    return bytes;
}

/// Printable C string starting at \p offset, stopping at the first NUL.
/// String pools concatenate several literals with NUL separators; a GEP into
/// such a pool selects one of them by offset.
std::string stringAt(const std::string& bytes, uint64_t offset) {
    if (offset >= bytes.size())
        return {};
    std::string out;
    for (uint64_t i = offset; i < bytes.size() && out.size() < 96; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0)
            break;
        if (c < 0x20 || c > 0x7e)
            return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

/// Extract a dispatch-table label from a constant. Strings are returned as-is,
/// integral keys as "#<value>". Returns empty for anything else.
std::string extractLabel(const llvm::Constant* C, const llvm::DataLayout& DL,
                         unsigned depth = 0) {
    if (!C || depth > 4)
        return {};
    if (llvm::isa<llvm::ConstantDataArray>(C) ||
        llvm::isa<llvm::GlobalVariable>(C))
        return stringAt(constantBytes(C), 0);
    if (const auto* CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
        // Vtable offsets / negative thunk adjustments are huge unsigned values;
        // they are padding, not dispatch keys.
        if (CI->getBitWidth() > 64 || CI->getZExtValue() > 0x7fffffffULL)
            return {};
        return "#" + std::to_string(CI->getZExtValue());
    }
    if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(C))
        return extractLabel(alias->getAliasee(), DL, depth + 1);
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(C)) {
        llvm::APInt offset(
            DL.getIndexSizeInBits(gep->getPointerAddressSpace()), 0);
        if (!gep->accumulateConstantOffset(DL, offset))
            return {};
        const auto* base =
            llvm::dyn_cast<llvm::Constant>(gep->getPointerOperand());
        return stringAt(constantBytes(base), offset.getZExtValue());
    }
    if (const auto* CE = llvm::dyn_cast<llvm::ConstantExpr>(C)) {
        switch (CE->getOpcode()) {
        case llvm::Instruction::BitCast:
        case llvm::Instruction::PtrToInt:
        case llvm::Instruction::IntToPtr:
        case llvm::Instruction::AddrSpaceCast:
            return extractLabel(CE->getOperand(0), DL, depth + 1);
        default:
            return {};
        }
    }
    return {};
}

std::string joinLabels(const std::vector<std::string>& labels) {
    std::vector<std::string> strings, numbers;
    for (const std::string& label : labels) {
        std::vector<std::string>& bucket =
            (!label.empty() && label[0] == '#') ? numbers : strings;
        if (std::find(bucket.begin(), bucket.end(), label) == bucket.end())
            bucket.push_back(label);
    }
    // A human-readable path/method beats the integral padding fields that
    // structs and PMF pairs carry; integer keys are still reported when the
    // table has no string label at all.
    const std::vector<std::string>& chosen = strings.empty() ? numbers : strings;
    std::string out;
    for (size_t i = 0; i < chosen.size() && i < 4 && out.size() < 120; ++i) {
        if (!out.empty())
            out += "|";
        out += sanitize(chosen[i]);
    }
    return out;
}

void addHandler(HandlerMap& handlers, const llvm::Function* F,
                HandlerKind kind, const std::string& trigger) {
    if (!F)
        return;
    auto it = handlers.find(F);
    if (it == handlers.end()) {
        handlers.emplace(F, HandlerRecord{kind, sanitize(trigger)});
        return;
    }
    HandlerRecord& record = it->second;
    if (static_cast<int>(kind) < static_cast<int>(record.kind))
        record.kind = kind;
    const std::string clean = sanitize(trigger);
    if (clean.empty() || record.trigger.find(clean) != std::string::npos)
        return;
    if (record.trigger.size() + clean.size() > 200)
        return;
    if (!record.trigger.empty())
        record.trigger += "|";
    record.trigger += clean;
}

void mergeHandlers(HandlerMap& dst, const HandlerMap& src) {
    for (const auto& item : src)
        addHandler(dst, item.first, item.second.kind, item.second.trigger);
}

/// Symbols that are never application callbacks: the C++ runtime, sanitizer
/// hooks, the standard/Boost library families and compiler-generated ctor
/// stubs. Used instead of requiring debug info, so release bitcode keeps its
/// callbacks. `FunctionFilter::isAppFunction` still gives a positive answer
/// when debug info is present.
bool looksLikeRuntimeSymbol(const std::string& mangled,
                            const std::string& demangled) {
    // Parse the outermost Itanium namespace component. A demangled name can
    // start with a return type ("bool boost::asio::..."), so prefix tests on
    // the demangled spelling alone are not enough.
    if (mangled.rfind("_Z", 0) == 0) {
        size_t i = 2;
        while (i < mangled.size() && mangled[i] == 'Z')
            ++i;
        if (i < mangled.size() && mangled[i] == 'N') {
            ++i;
            while (i < mangled.size() &&
                   (mangled[i] == 'K' || mangled[i] == 'V' ||
                    mangled[i] == 'r' || mangled[i] == 'R' ||
                    mangled[i] == 'O'))
                ++i;
            if (mangled.compare(i, 2, "St") == 0)
                return true;
            size_t len = 0;
            while (i < mangled.size() &&
                   std::isdigit(static_cast<unsigned char>(mangled[i]))) {
                len = len * 10 + static_cast<size_t>(mangled[i] - '0');
                ++i;
            }
            if (len && i + len <= mangled.size()) {
                const std::string ns = mangled.substr(i, len);
                if (ns == "boost" || ns == "std" || ns == "__gnu_cxx" ||
                    ns == "__cxxabiv1")
                    return true;
            }
        }
    }

    static const char* kMangledPrefixes[] = {
        "_ZSt", "_ZNSt", "_ZNKSt", "_ZNS", "_ZNSa", "_ZN9__gnu_cxx",
        "_ZNK9__gnu_cxx"};
    for (const char* p : kMangledPrefixes)
        if (mangled.rfind(p, 0) == 0)
            return true;

    static const char* kNames[] = {
        "std::", "__gnu_cxx::", "boost::", "__cxa_", "__gxx_", "_Unwind_",
        "__asan_", "__ubsan_", "__tsan_", "__msan_", "__sanitizer_",
        "__stack_chk_", "__fortify_", "_GLOBAL__sub_I_",
        "__cxx_global_var_init", "__clang_call_terminate", "operator new",
        "operator delete", "llvm.", "non-virtual thunk to ",
        "virtual thunk to "};
    for (const char* p : kNames)
        if (demangled.rfind(p, 0) == 0)
            return true;
    return false;
}

/// Itanium vtable / RTTI / construction-vtable / guard-variable globals are
/// compiler metadata, not dispatch tables. Their integral slots (vcall and
/// vbase offsets) would otherwise look like integral route keys.
bool isRuntimeMetadataGlobal(llvm::StringRef name) {
    static const char* kPrefixes[] = {"_ZTV", "_ZTI", "_ZTS", "_ZTC",
                                      "_ZTT", "_ZGV"};
    for (const char* p : kPrefixes)
        if (name.starts_with(p))
            return true;
    return false;
}

/// Compiler-generated initialisation stubs are never business entries; the
/// handlers they register are reported separately as registered-handler.
bool isCompilerInitStub(const std::string& raw) {
    return raw.rfind("_GLOBAL__sub_I_", 0) == 0 ||
           raw.rfind("__cxx_global_var_init", 0) == 0;
}

/// Collect functions referenced by a value: a plain function pointer, a
/// bitcast / ptrtoint expression, a GlobalAlias, or a global whose initializer
/// is exactly such a pointer. Only unary conversion chains are followed —
/// walking an arbitrary aggregate (std::map internals, vtables) would register
/// unrelated code.
void collectFunctionRefs(const llvm::Value* V, unsigned depth,
                         std::vector<const llvm::Function*>& out) {
    if (!V || depth > 4)
        return;
    if (const auto* F = llvm::dyn_cast<llvm::Function>(V)) {
        out.push_back(F);
        return;
    }
    if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(V)) {
        collectFunctionRefs(alias->getAliasee(), depth + 1, out);
        return;
    }
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
        if (!GV->hasInitializer())
            return;
        const llvm::Value* stripped = GV->getInitializer()->stripPointerCasts();
        if (llvm::isa<llvm::Function>(stripped) ||
            llvm::isa<llvm::ConstantExpr>(stripped) ||
            llvm::isa<llvm::GlobalAlias>(stripped))
            collectFunctionRefs(stripped, depth + 1, out);
        return;
    }
    if (const auto* CE = llvm::dyn_cast<llvm::ConstantExpr>(V)) {
        if (CE->getNumOperands() != 1)
            return;
        switch (CE->getOpcode()) {
        case llvm::Instruction::BitCast:
        case llvm::Instruction::PtrToInt:
        case llvm::Instruction::IntToPtr:
        case llvm::Instruction::AddrSpaceCast:
        case llvm::Instruction::ZExt:
        case llvm::Instruction::SExt:
        case llvm::Instruction::Trunc:
            collectFunctionRefs(CE->getOperand(0), depth + 1, out);
            return;
        default:
            return;
        }
    }
    if (const auto* I = llvm::dyn_cast<llvm::Instruction>(V)) {
        if ((llvm::isa<llvm::CastInst>(I) ||
             llvm::isa<llvm::GEPOperator>(I)) &&
            I->getNumOperands() >= 1)
            collectFunctionRefs(I->getOperand(0), depth + 1, out);
    }
}

/// Collect functions from a constant expression tree, including globals
/// referenced by the table (their initializer may hold the handler pointer).
/// Labels are never propagated through those globals: vtables reference RTTI
/// objects whose type name is a string, which must not turn a vtable into a
/// route table.
void collectConstantFunctions(const llvm::Constant* C, unsigned depth,
                              std::unordered_set<const llvm::Constant*>& visiting,
                              std::vector<const llvm::Function*>& out) {
    if (!C || depth > 8 || !visiting.insert(C).second)
        return;
    if (const auto* F = llvm::dyn_cast<llvm::Function>(C)) {
        out.push_back(F);
        visiting.erase(C);
        return;
    }
    if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(C))
        collectConstantFunctions(alias->getAliasee(), depth + 1, visiting, out);
    else if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
        if (GV->hasInitializer() && depth < 4)
            collectConstantFunctions(GV->getInitializer(), depth + 1, visiting,
                                     out);
    } else if (!llvm::isa<llvm::ConstantData>(C) &&
               !llvm::isa<llvm::ConstantAggregateZero>(C))
        for (const llvm::Use& U : C->operands())
            if (const auto* child = llvm::dyn_cast<llvm::Constant>(U.get()))
                collectConstantFunctions(child, depth + 1, visiting, out);
    visiting.erase(C);
}

//===----------------------------------------------------------------------===//
// Constant dispatch tables + LLVM metadata globals.
//===----------------------------------------------------------------------===//

class DispatchTableScanner {
public:
    explicit DispatchTableScanner(const llvm::Module& M)
        : M_(M), DL_(M.getDataLayout()) {}

    HandlerMap run() {
        scanModule();
        return handlers_;
    }

    const std::unordered_set<const llvm::Function*>& initializers() const {
        return initializers_;
    }

private:
    void scanModule() {
        for (const llvm::GlobalVariable& GV : M_.globals()) {
            if (!GV.hasInitializer())
                continue;
            const llvm::StringRef name = GV.getName();
            if (name == "llvm.global_ctors" || name == "llvm.global_dtors")
                collectInitializers(GV.getInitializer());
            else if (name == "llvm.global.annotations")
                collectAnnotations(GV.getInitializer());
            else if (name == "llvm.used" || name == "llvm.compiler.used")
                collectRetained(GV.getInitializer(), name.str());
            else if (name.starts_with("llvm.") || isRuntimeMetadataGlobal(name))
                continue;  // debug info, vtables, RTTI and other compiler data
            else {
                std::unordered_set<const llvm::Constant*> visiting;
                Result root = visit(GV.getInitializer(), true, 0, visiting);
                registerCandidates(root.candidates);
            }
        }
    }

    /// One aggregate that pairs labels with handler pointers. Candidates from
    /// every nesting level are collected, then the most specific one wins for
    /// each function (see registerCandidates).
    struct Candidate {
        std::vector<const llvm::Function*> functions;
        std::vector<std::string> labels;
    };
    struct Result {
        std::vector<const llvm::Function*> functions;
        std::vector<std::string> labels;
        std::vector<Candidate> candidates;
    };

    static size_t uniqueLabelCount(const std::vector<std::string>& labels) {
        std::unordered_set<std::string> unique(labels.begin(), labels.end());
        return unique.size();
    }

    static bool hasStringLabel(const std::vector<std::string>& labels) {
        for (const std::string& label : labels)
            if (!label.empty() && label[0] != '#')
                return true;
        return false;
    }

    /// A readable route label beats integral padding, and a narrow (innermost)
    /// aggregate beats the enclosing array that lists every route.
    static bool better(const Candidate& a, const Candidate& b) {
        const bool aString = hasStringLabel(a.labels);
        const bool bString = hasStringLabel(b.labels);
        if (aString != bString)
            return aString;
        return uniqueLabelCount(a.labels) < uniqueLabelCount(b.labels);
    }

    void registerCandidates(const std::vector<Candidate>& candidates) {
        std::unordered_map<const llvm::Function*, const Candidate*> best;
        for (const Candidate& candidate : candidates)
            for (const llvm::Function* F : candidate.functions) {
                auto it = best.find(F);
                if (it == best.end() || better(candidate, *it->second))
                    best[F] = &candidate;
            }
        for (const auto& item : best)
            addHandler(handlers_, item.first, HandlerKind::RouteTable,
                       joinLabels(item.second->labels));
    }

    const llvm::Module& M_;
    const llvm::DataLayout& DL_;
    HandlerMap handlers_;
    std::unordered_set<const llvm::Function*> initializers_;

    /// Walk a constant aggregate tree, collecting labels, handler pointers and
    /// one registration candidate per level that pairs them.
    Result visit(const llvm::Constant* C, bool labelsAllowed, unsigned depth,
                 std::unordered_set<const llvm::Constant*>& visiting) {
        Result result;
        if (!C || depth > 8)
            return result;
        if (const auto* F = llvm::dyn_cast<llvm::Function>(C)) {
            result.functions.push_back(F);
            return result;
        }
        if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(C))
            return visit(alias->getAliasee(), labelsAllowed, depth + 1,
                         visiting);

        if (labelsAllowed) {
            const std::string label = extractLabel(C, DL_);
            if (!label.empty())
                result.labels.push_back(label);
        }
        // A GEP into a string pool already carries the precise label; its base
        // global and index would only add the pool's first string and "#N".
        const bool childLabelsAllowed = labelsAllowed && result.labels.empty();

        if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
            // Function pointers may live in a separate global constant. Follow
            // it for functions only; labels must come from this table's own
            // operands (otherwise every vtable would look like a route table).
            if (GV->hasInitializer() && depth < 4 &&
                visiting.insert(GV).second) {
                Result sub = visit(GV->getInitializer(), false, depth + 1,
                                   visiting);
                result.functions.insert(result.functions.end(),
                                        sub.functions.begin(),
                                        sub.functions.end());
                visiting.erase(GV);
            }
            return result;
        }

        if (llvm::isa<llvm::ConstantData>(C) ||
            llvm::isa<llvm::ConstantAggregateZero>(C) ||
            llvm::isa<llvm::ConstantPointerNull>(C))
            return result;

        for (const llvm::Use& U : C->operands()) {
            const auto* child = llvm::dyn_cast<llvm::Constant>(U.get());
            if (!child || child == C)
                continue;
            Result sub = visit(child, childLabelsAllowed, depth + 1, visiting);
            result.functions.insert(result.functions.end(),
                                    sub.functions.begin(), sub.functions.end());
            result.labels.insert(result.labels.end(), sub.labels.begin(),
                                 sub.labels.end());
            result.candidates.insert(result.candidates.end(),
                                     sub.candidates.begin(),
                                     sub.candidates.end());
        }
        if (labelsAllowed && !result.labels.empty() &&
            !result.functions.empty())
            result.candidates.push_back({result.functions, result.labels});
        return result;
    }

    void collectInitializers(const llvm::Constant* init) {
        std::vector<const llvm::Function*> funcs;
        std::unordered_set<const llvm::Constant*> visiting;
        collectConstantFunctions(init, 0, visiting, funcs);
        for (const llvm::Function* F : funcs)
            initializers_.insert(F);
    }

    void collectAnnotations(const llvm::Constant* init) {
        if (!init)
            return;
        for (const llvm::Use& U : init->operands()) {
            const auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(U.get());
            if (!entry || entry->getNumOperands() < 2)
                continue;
            const auto* target =
                llvm::dyn_cast<llvm::Constant>(entry->getOperand(0));
            const auto* text =
                llvm::dyn_cast<llvm::Constant>(entry->getOperand(1));
            if (!target)
                continue;
            std::vector<const llvm::Function*> funcs;
            std::unordered_set<const llvm::Constant*> visiting;
            collectConstantFunctions(target, 0, visiting, funcs);
            std::string trigger = text ? extractLabel(text, DL_) : std::string();
            if (trigger.empty())
                trigger = "annotation";
            for (const llvm::Function* F : funcs)
                addHandler(handlers_, F, HandlerKind::Annotation, trigger);
        }
    }

    void collectRetained(const llvm::Constant* init, const std::string& name) {
        std::vector<const llvm::Function*> funcs;
        std::unordered_set<const llvm::Constant*> visiting;
        collectConstantFunctions(init, 0, visiting, funcs);
        for (const llvm::Function* F : funcs)
            addHandler(handlers_, F, HandlerKind::Retained, name);
    }
};

//===----------------------------------------------------------------------===//
// Run-time registration: function pointers stored or handed to a framework.
//===----------------------------------------------------------------------===//

bool isGlobalCtorName(llvm::StringRef name) {
    return name.starts_with("_GLOBAL__sub_I_") ||
           name.starts_with("__cxx_global_var_init");
}

bool pointsToLocalAlloca(const llvm::Value* V, unsigned depth = 0) {
    if (!V || depth > 6)
        return false;
    V = V->stripPointerCasts();
    if (llvm::isa<llvm::AllocaInst>(V))
        return true;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(V))
        return pointsToLocalAlloca(gep->getPointerOperand(), depth + 1);
    return false;
}

std::string readableName(llvm::StringRef mangled) {
    std::string out = mangled.starts_with("_Z")
                          ? stripParams(llvm::demangle(mangled.str()))
                          : mangled.str();
    // Template instantiations make registration contexts unreadably long.
    if (out.size() > 64)
        out.resize(64);
    return out;
}

class RegistrationScanner {
public:
    explicit RegistrationScanner(
        const llvm::Module& M,
        const std::unordered_set<const llvm::Function*>& initializers,
        bool includeLibrary)
        : M_(M), DL_(M.getDataLayout()), initializers_(initializers),
          includeLibrary_(includeLibrary) {}

    HandlerMap run() {
        for (const llvm::Function& F : M_) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            const bool inCtor = initializers_.count(&F) != 0 ||
                                isGlobalCtorName(F.getName());
            for (const llvm::Instruction& I : llvm::instructions(F)) {
                if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                    const llvm::Function* callee = CB->getCalledFunction();
                    if (callee && callee->isIntrinsic())
                        continue;
                    const std::string context = describeCall(CB, inCtor);
                    for (unsigned a = 0; a < CB->arg_size(); ++a)
                        record(CB->getArgOperand(a), context);
                } else if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    if (pointsToLocalAlloca(SI->getPointerOperand()))
                        continue;  // a local fn-ptr variable is not a registry
                    record(SI->getValueOperand(), describeStore(SI, inCtor));
                }
            }
        }
        return handlers_;
    }

private:
    const llvm::Module& M_;
    const llvm::DataLayout& DL_;
    const std::unordered_set<const llvm::Function*>& initializers_;
    bool includeLibrary_;
    HandlerMap handlers_;

    void record(const llvm::Value* V, const std::string& context) {
        std::vector<const llvm::Function*> funcs;
        collectFunctionRefs(V, 0, funcs);
        for (const llvm::Function* F : funcs) {
            if (F->isDeclaration() || F->isIntrinsic())
                continue;
            if (!includeLibrary_) {
                const std::string raw = F->getName().str();
                if (looksLikeRuntimeSymbol(raw, llvm::demangle(raw)))
                    continue;
            }
            addHandler(handlers_, F, HandlerKind::Registration, context);
        }
    }

    static std::string prefix(bool inCtor) {
        return inCtor ? "ctor:" : "";
    }

    std::string describeCall(const llvm::CallBase* CB, bool inCtor) const {
        const llvm::Function* callee = CB->getCalledFunction();
        std::string context = prefix(inCtor) + "call " +
                              (callee ? readableName(callee->getName())
                                      : std::string("indirect"));
        std::vector<std::string> labels;
        for (unsigned a = 0; a < CB->arg_size(); ++a)
            if (const auto* C =
                    llvm::dyn_cast<llvm::Constant>(CB->getArgOperand(a))) {
                const std::string label = extractLabel(C, DL_);
                if (!label.empty() && label[0] != '#')
                    labels.push_back(label);
            }
        const std::string joined = joinLabels(labels);
        if (!joined.empty())
            context += "(" + joined + ")";
        return context;
    }

    std::string describeStore(const llvm::StoreInst* SI, bool inCtor) const {
        std::string context = prefix(inCtor) + "store";
        const llvm::Value* ptr = SI->getPointerOperand()->stripPointerCasts();
        if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(ptr))
            context += " @" + GV->getName().str();
        if (const llvm::BasicBlock* bb = SI->getParent()) {
            std::string label;
            for (const llvm::Instruction& prev : *bb) {
                if (&prev == SI)
                    break;
                for (const llvm::Use& U : prev.operands())
                    if (const auto* C =
                            llvm::dyn_cast<llvm::Constant>(U.get())) {
                        const std::string candidate = extractLabel(C, DL_);
                        if (!candidate.empty() && candidate[0] != '#')
                            label = candidate;
                    }
            }
            if (!label.empty())
                context += "(" + sanitize(label) + ")";
        }
        return context;
    }
};

//===----------------------------------------------------------------------===//
// Direct-call graph over the functions defined in the module. Node ids are
// indices into `functions`.
//===----------------------------------------------------------------------===//

struct ModuleGraph {
    std::vector<const llvm::Function*> functions;
    std::vector<std::vector<int>> callees;
    std::vector<bool> addressTaken;
};

ModuleGraph buildGraph(const llvm::Module& M) {
    ModuleGraph g;
    std::unordered_map<const llvm::Function*, int> ids;
    for (const llvm::Function& F : M) {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;
        ids.emplace(&F, static_cast<int>(g.functions.size()));
        g.functions.push_back(&F);
    }
    g.callees.resize(g.functions.size());
    g.addressTaken.assign(g.functions.size(), false);

    for (size_t i = 0; i < g.functions.size(); ++i) {
        const llvm::Function* F = g.functions[i];

        // Direct call/invoke/callbr edges. Indirect calls cannot be resolved
        // here; their potential targets are flagged via addressTaken below.
        for (const llvm::Instruction& I : llvm::instructions(F)) {
            const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!CB || !CB->getCalledOperand())
                continue;
            const auto* callee = llvm::dyn_cast<llvm::Function>(
                CB->getCalledOperand()->stripPointerCasts());
            if (!callee)
                continue;
            auto it = ids.find(callee);
            if (it != ids.end())
                g.callees[i].push_back(it->second);
        }

        // A use other than the callee operand of a direct call means the
        // function's address escapes into a pointer, global (vtable /
        // llvm.global_ctors), or an argument: it may be called from anywhere.
        for (const llvm::Use& U : F->uses()) {
            const auto* CB = llvm::dyn_cast<llvm::CallBase>(U.getUser());
            if (CB && CB->isCallee(&U))
                continue;
            g.addressTaken[i] = true;
        }
    }
    return g;
}

/// Tarjan strongly connected components, iterative so that a deep call chain
/// in whole-program bitcode cannot overflow the C++ stack.
std::vector<int> computeSccs(const std::vector<std::vector<int>>& edges,
                             int& sccCount) {
    const int n = static_cast<int>(edges.size());
    std::vector<int> index(n, -1), low(n, -1), comp(n, -1), stack;
    std::vector<char> onStack(n, 0);
    struct Frame {
        int node;
        size_t nextEdge;
    };
    std::vector<Frame> frames;
    int nextIndex = 0;
    sccCount = 0;

    for (int start = 0; start < n; ++start) {
        if (index[start] != -1)
            continue;
        index[start] = low[start] = nextIndex++;
        stack.push_back(start);
        onStack[start] = 1;
        frames.push_back({start, 0});

        while (!frames.empty()) {
            Frame& frame = frames.back();
            const int v = frame.node;
            if (frame.nextEdge < edges[v].size()) {
                const int w = edges[v][frame.nextEdge++];
                if (index[w] == -1) {
                    index[w] = low[w] = nextIndex++;
                    stack.push_back(w);
                    onStack[w] = 1;
                    frames.push_back({w, 0});
                } else if (onStack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
                continue;
            }
            frames.pop_back();
            if (!frames.empty()) {
                const int parent = frames.back().node;
                low[parent] = std::min(low[parent], low[v]);
            }
            if (low[v] == index[v]) {
                while (true) {
                    const int w = stack.back();
                    stack.pop_back();
                    onStack[w] = 0;
                    comp[w] = sccCount;
                    if (w == v)
                        break;
                }
                ++sccCount;
            }
        }
    }
    return comp;
}

//===----------------------------------------------------------------------===//
// Entry selection.
//===----------------------------------------------------------------------===//

struct Entry {
    std::string name;
    const char* reason;
    std::string detail;
};

int reasonRank(const char* reason) {
    static const char* kOrder[] = {
        "program-entry", "route-handler", "registered-handler", "annotated",
        "retained",      "callback",      "no-caller",          "exported",
        "defined"};
    for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i)
        if (std::strcmp(reason, kOrder[i]) == 0)
            return i;
    return 99;
}

std::vector<Entry> collectEntries(const llvm::Module& M, const Options& opt) {
    const ModuleGraph g = buildGraph(M);

    DispatchTableScanner tables(M);
    HandlerMap handlers = tables.run();
    mergeHandlers(handlers,
                  RegistrationScanner(M, tables.initializers(),
                                      opt.includeLibrary).run());
    // Library/framework internals that happen to sit in a vtable or get passed
    // to a registration API are not application entries.
    if (!opt.includeLibrary)
        for (auto it = handlers.begin(); it != handlers.end();) {
            const std::string raw = it->first->getName().str();
            if (looksLikeRuntimeSymbol(raw, llvm::demangle(raw)))
                it = handlers.erase(it);
            else
                ++it;
        }

    // An SCC is a root group when no direct call from outside that SCC reaches
    // it. An escaping address no longer disqualifies it: indirect dispatch is
    // exactly how HTTP handlers and other framework callbacks are invoked.
    int sccCount = 0;
    std::vector<int> comp;
    std::vector<char> hasExternalCaller;
    if (opt.mode == Mode::Roots) {
        comp = computeSccs(g.callees, sccCount);
        hasExternalCaller.assign(sccCount, 0);
        for (size_t u = 0; u < g.callees.size(); ++u)
            for (int v : g.callees[u])
                if (comp[u] != comp[v])
                    hasExternalCaller[comp[v]] = 1;
    }

    // Debug info is an enhancement, not a prerequisite: callbacks are kept by
    // default when their symbol is not plainly library/runtime code. With -g,
    // FunctionFilter additionally removes out-of-tree framework templates.
    std::unique_ptr<FunctionFilter> filter;
    if (opt.mode == Mode::Roots && !opt.includeLibrary)
        filter = std::make_unique<FunctionFilter>(FunctionFilter::build(M));
    const bool hasDebugInfo = M.getNamedMetadata("llvm.dbg.cu") != nullptr;

    std::vector<Entry> entries;
    for (size_t i = 0; i < g.functions.size(); ++i) {
        const llvm::Function* F = g.functions[i];
        const std::string raw = F->getName().str();
        const std::string demangled = llvm::demangle(raw);
        const char* reason = nullptr;
        std::string detail;
        const auto hit = handlers.find(F);
        switch (opt.mode) {
        case Mode::Roots:
            if (raw == "main" || raw == "svf.main") {
                reason = "program-entry";
            } else if (hit != handlers.end()) {
                reason = kindReason(hit->second.kind);
                detail = hit->second.trigger;
            } else if (!hasExternalCaller[comp[i]]) {
                if (!opt.includeLibrary && isCompilerInitStub(raw))
                    continue;
                if (g.addressTaken[i] && !opt.includeLibrary) {
                    const bool appByDebug =
                        hasDebugInfo && filter && filter->isAppFunction(raw);
                    const bool excludedByDebug =
                        hasDebugInfo && filter &&
                        filter->isExcluded(raw, demangled);
                    if (!appByDebug &&
                        (looksLikeRuntimeSymbol(raw, demangled) ||
                         excludedByDebug))
                        continue;
                }
                reason = g.addressTaken[i] ? "callback" : "no-caller";
            }
            break;
        case Mode::Routes:
            if (hit != handlers.end()) {
                reason = kindReason(hit->second.kind);
                detail = hit->second.trigger;
            }
            break;
        case Mode::Exported:
            if (!F->hasLocalLinkage())
                reason = "exported";
            break;
        case Mode::All:
            reason = "defined";
            break;
        }
        if (!reason)
            continue;
        entries.push_back({opt.demangle ? demangled : raw, reason,
                           std::move(detail)});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.name != b.name)
                      return a.name < b.name;
                  return reasonRank(a.reason) < reasonRank(b.reason);
              });
    // Demangling can collapse distinct symbols (e.g. the D0/D1 destructor
    // variants) into the same name; a name list only needs one of each. Keep
    // the most specific reason and merge all triggers.
    std::vector<Entry> merged;
    merged.reserve(entries.size());
    for (Entry& e : entries) {
        if (!merged.empty() && merged.back().name == e.name) {
            Entry& keep = merged.back();
            if (!e.detail.empty() &&
                keep.detail.find(e.detail) == std::string::npos &&
                keep.detail.size() + e.detail.size() < 200) {
                if (!keep.detail.empty())
                    keep.detail += "|";
                keep.detail += e.detail;
            }
        } else {
            merged.push_back(std::move(e));
        }
    }
    return merged;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (std::strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc) {
                llvm::errs() << "[ERROR] --mode needs a value\n";
                return 1;
            }
            const char* value = argv[++i];
            if (std::strcmp(value, "roots") == 0)
                opt.mode = Mode::Roots;
            else if (std::strcmp(value, "routes") == 0)
                opt.mode = Mode::Routes;
            else if (std::strcmp(value, "exported") == 0)
                opt.mode = Mode::Exported;
            else if (std::strcmp(value, "all") == 0)
                opt.mode = Mode::All;
            else {
                llvm::errs() << "[ERROR] unknown --mode '" << value
                             << "' (expected roots, routes, exported or all)\n";
                return 1;
            }
        } else if (std::strcmp(arg, "--include-library") == 0) {
            opt.includeLibrary = true;
        } else if (std::strcmp(arg, "--demangle") == 0) {
            opt.demangle = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            opt.verbose = true;
        } else if (std::strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                llvm::errs() << "[ERROR] --output needs a file path\n";
                return 1;
            }
            opt.outputFile = argv[++i];
        } else if (arg[0] == '-') {
            llvm::errs() << "[ERROR] unknown option '" << arg << "'\n";
            return 1;
        } else {
            opt.bcFile = arg;
        }
    }

    if (opt.bcFile.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    llvm::SMDiagnostic diagnostic;
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(opt.bcFile, diagnostic, context);
    if (!module) {
        llvm::errs() << "[ERROR] failed to load bitcode file: " << opt.bcFile
                     << "\n";
        diagnostic.print(argv[0], llvm::errs());
        return 1;
    }

    const std::vector<Entry> entries = collectEntries(*module, opt);

    std::ofstream file;
    std::ostream* out = &std::cout;
    if (!opt.outputFile.empty()) {
        file.open(opt.outputFile);
        if (!file) {
            llvm::errs() << "[ERROR] cannot open output file: "
                         << opt.outputFile << "\n";
            return 1;
        }
        out = &file;
    }
    for (const Entry& entry : entries) {
        *out << entry.name;
        if (opt.verbose) {
            *out << '\t' << entry.reason;
            if (!entry.detail.empty())
                *out << '\t' << entry.detail;
        }
        *out << '\n';
    }
    if (out->fail()) {
        llvm::errs() << "[ERROR] failed to write output\n";
        return 1;
    }

    if (opt.verbose) {
        size_t defined = 0;
        for (const llvm::Function& F : *module)
            if (!F.isDeclaration() && !F.isIntrinsic())
                ++defined;
        llvm::errs() << "[INFO] " << opt.bcFile << ": " << defined
                     << " defined functions, " << entries.size()
                     << " entries (mode=" << modeName(opt.mode) << ")\n";
    }
    return 0;
}
