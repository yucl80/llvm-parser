//===----------------------------------------------------------------------===//
// entry_extractor.cpp — list the entry functions of LLVM bitcode modules.
//
// Usage: entry_extractor [options] <bitcode-file> [<bitcode-file> ...]
//
// "Entry function" has several readings; --mode selects one:
//   roots (default)  Functions that can be triggered from outside the module's
//                    own reachable direct call graph:
//                      * program entries (main, svf.main, framework entry
//                        names supplied with --entry-name);
//                      * framework-registered handlers (see below);
//                      * functions with no reachable direct caller:
//                        uncalled functions ("no-caller") and functions whose
//                        address escapes ("callback").
//                    Recursion and mutual recursion are handled with SCCs.
//   routes           Only the framework-registered handlers.
//   exported         Every defined function whose linkage AND visibility make
//                    it visible to other modules (plus alias/ifunc targets).
//   all              Every function with a body (inspection / debugging).
//
// A "framework-registered handler" is recognised in several ways:
//   route-handler       A constant dispatch table pairing a label with a
//                       function pointer. Labels may be C strings (UTF-8,
//                       merged string pools / GEPs, wide strings), integral
//                       keys (including negative / 64-bit opcodes) or floats.
//   registered-handler A function pointer stored or passed to a registration
//                       API at run time. A local value-flow resolver follows
//                       loads, phi/select, function-returned pointers,
//                       aggregate constants, GEPs and memcpy-style copies.
//   type-erased        Lambda / std::function / std::bind targets recovered
//                       from the standard library's invoker chain.
//   annotated          @llvm.global.annotations or a function's !annotation.
//   retained           @llvm.used / @llvm.compiler.used / @llvm.global_dtors.
//   section-handler    A function pointer in a custom section (.init_array,
//                       linker sets, ...).
//   aliased            Target of an externally visible GlobalAlias / ifunc.
//   virtual-handler    Opt-in (--virtual-handlers): functions in _ZTV* tables.
//
// Output is a plain name list by default (feed it to
// `clang_analyzer --entry-file <file>`), or structured JSON with --format json
// carrying mangled/demangled names, source location and signature.
// Declarations without a body are never printed: they are library calls.
//
// Optimised (-O2/LTO) bitcode may have folded the constant tables and
// registration calls this scanner relies on; --verbose / JSON emit a warning.
// Prefer -O0 / pre-LTO bitcode when completeness matters.
//===----------------------------------------------------------------------===//

#include "function_filter.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Use.h>
#include <llvm/IR/User.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum class Mode { Roots, Routes, Exported, All };
enum class OutputFormat { Names, Json };

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
    OutputFormat format = OutputFormat::Names;
    bool demangle = false;
    bool verbose = false;
    bool includeLibrary = false;
    bool allLabels = false;
    bool virtualHandlers = false;
    bool typeErased = true;
    bool sectionHandlers = true;
    bool reachableRoots = true;
    std::string outputFile;
    std::string filterFile;
    std::vector<std::string> bcFiles;
    std::vector<std::string> appSourceRoots;
    std::vector<std::string> namePatterns;
    std::vector<std::string> extraEntryNames;
};

void printUsage(const char* prog) {
    llvm::errs()
        << "Usage: " << prog
        << " [options] <bitcode-file> [<bitcode-file> ...]\n"
        << "Options:\n"
        << "  --mode <roots|routes|exported|all>\n"
        << "                     Which functions count as entries:\n"
        << "                       roots    - program entries, framework\n"
        << "                                  handlers and functions with no\n"
        << "                                  reachable direct caller [default]\n"
        << "                       routes   - only framework-registered\n"
        << "                                  handlers (route tables, runtime\n"
        << "                                  registration, annotations)\n"
        << "                       exported- every defined function visible\n"
        << "                                  to other modules\n"
        << "                       all      - every function with a body\n"
        << "  --format <names|json>\n"
        << "                     Plain name list [default] or structured JSON\n"
        << "                     with mangled name, location and signature\n"
        << "  --include-library  Keep library/framework callbacks as entries\n"
        << "  --demangle         Print demangled C++ names (names format)\n"
        << "  --verbose          Append reason and trigger (tab-separated)\n"
        << "                     to each name and print a summary on stderr\n"
        << "  --output <file>    Write to <file> instead of stdout\n"
        << "  --app-source-root <dir>\n"
        << "                     Application source root (repeatable); enables\n"
        << "                     definition-site library classification\n"
        << "  --filter-file <f> One ECMAScript regex per line; matching\n"
        << "                     functions are excluded from the report\n"
        << "  --entry-name <n>  Extra program-entry name (repeatable; a\n"
        << "                     trailing '*' makes it a prefix match)\n"
        << "  --all-labels      Keep integer/float labels alongside strings\n"
        << "                     and scan labels across basic blocks\n"
        << "  --virtual-handlers\n"
        << "                     Report functions referenced by _ZTV* tables\n"
        << "  --no-type-erased  Disable lambda/std::function target recovery\n"
        << "  --no-section-handlers\n"
        << "                     Disable custom-section (.init_array) handlers\n"
        << "  --legacy-roots    Count dead-code direct callers as callers\n"
        << "  --help             Show this help\n";
}

//===----------------------------------------------------------------------===//
// Shared helpers: labels, function references and handler bookkeeping.
//===----------------------------------------------------------------------===//

/// Why a function was recognised as a framework entry. Ordered by confidence:
/// a route table beats a runtime registration, which beats a metadata hint.
enum class HandlerKind { RouteTable = 0, Registration = 1, Annotation = 2,
                         Retained = 3, Section = 4, Alias = 5, Virtual = 6 };

const char* kindReason(HandlerKind kind) {
    switch (kind) {
    case HandlerKind::RouteTable: return "route-handler";
    case HandlerKind::Registration: return "registered-handler";
    case HandlerKind::Annotation: return "annotated";
    case HandlerKind::Retained: return "retained";
    case HandlerKind::Section: return "section-handler";
    case HandlerKind::Alias: return "aliased";
    case HandlerKind::Virtual: return "virtual-handler";
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

/// Minimal JSON string escaping; the report contains user-controlled symbols.
std::string jsonEscape(llvm::StringRef s) {
    std::string out = "\"";
    char buf[7];
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out += '"';
    return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

/// Reject arbitrary binary data that happens to contain high bytes.
bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        unsigned need = 0;
        uint32_t cp = 0;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need >= s.size()) return false;
        for (unsigned k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += need + 1;
    }
    return true;
}

/// Bytes of a string constant. Handles plain i8 ConstantDataArray, a
/// ConstantArray of i8 elements, wide (i16/i32) strings encoded as UTF-8, and
/// any of them behind a global (`@.str`). A GEP/string-pool label does not
/// have to be NUL-terminated.
std::string constantBytes(const llvm::Constant* C) {
    if (!C)
        return {};
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
        if (!GV->hasInitializer())
            return {};
        C = GV->getInitializer();
    }
    const llvm::Value* stripped = C->stripPointerCasts();

    if (const auto* data =
            llvm::dyn_cast<llvm::ConstantDataArray>(stripped)) {
        llvm::Type* element = data->getElementType();
        if (element->isIntegerTy(8)) {
            std::string bytes;
            bytes.reserve(data->getNumElements());
            for (unsigned i = 0; i < data->getNumElements(); ++i)
                bytes.push_back(
                    static_cast<char>(data->getElementAsInteger(i) & 0xff));
            return bytes;
        }
        // wchar_t / char16_t / char32_t literals. Only plausible text is
        // accepted: a terminator (or a short array), no controls/surrogates.
        if (!element->isIntegerTy(16) && !element->isIntegerTy(32))
            return {};
        const unsigned n = data->getNumElements();
        if (n > 256)
            return {};
        bool hasTerminator = false;
        for (unsigned i = 0; i < n; ++i)
            if (data->getElementAsInteger(i) == 0) {
                hasTerminator = true;
                break;
            }
        if (!hasTerminator && n > 32)
            return {};
        std::string out;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t cp = data->getElementAsInteger(i);
            if (cp == 0)
                break;
            if (cp < 0x20 || cp > 0x10ffff ||
                (cp >= 0xd800 && cp <= 0xdfff))
                return {};
            appendUtf8(out, static_cast<uint32_t>(cp));
        }
        return out;
    }

    if (const auto* array = llvm::dyn_cast<llvm::ConstantArray>(stripped)) {
        if (!array->getType()->getElementType()->isIntegerTy(8))
            return {};
        std::string bytes;
        bytes.reserve(array->getNumOperands());
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
            const auto* CI =
                llvm::dyn_cast<llvm::ConstantInt>(array->getOperand(i));
            if (!CI)
                return {};
            bytes.push_back(static_cast<char>(CI->getZExtValue() & 0xff));
        }
        return bytes;
    }
    return {};
}

/// Printable string starting at \p offset, stopping at the first NUL.
/// Non-ASCII UTF-8 bytes are kept (a Chinese route label is a valid label);
/// binary data with high bytes is rejected instead of becoming a label.
std::string stringAt(const std::string& bytes, uint64_t offset) {
    if (offset >= bytes.size())
        return {};
    std::string out;
    for (uint64_t i = offset; i < bytes.size() && out.size() < 256; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0)
            break;
        if (c < 0x20 || c == 0x7f)
            return {};
        out.push_back(static_cast<char>(c));
    }
    return validUtf8(out) ? out : std::string();
}

/// Extract a dispatch-table label from a constant. Strings are returned as-is,
/// integral keys as "#<value>" (signed, so negative opcodes survive), floats as
/// "#f<value>". Returns empty for anything else.
std::string extractLabel(const llvm::Constant* C, const llvm::DataLayout& DL,
                         unsigned depth = 0) {
    if (!C || depth > 5)
        return {};
    if (llvm::isa<llvm::ConstantDataArray>(C) ||
        llvm::isa<llvm::ConstantArray>(C) ||
        llvm::isa<llvm::GlobalVariable>(C)) {
        const std::string text = stringAt(constantBytes(C), 0);
        if (!text.empty())
            return text;
        // Never descend into a global's aggregate operands here: RTTI objects
        // carry type-name strings that would turn every vtable into a table.
        if (llvm::isa<llvm::GlobalVariable>(C))
            return {};
    }
    if (const auto* CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
        if (CI->getBitWidth() > 64)
            return {};
        // Huge unsigned values are vtable offsets / thunk adjustments, not
        // dispatch keys. Everything else (including i32 -1 and 64-bit ids) is
        // a plausible opcode or message id.
        if (CI->getZExtValue() >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return {};
        const int64_t value = CI->getSExtValue();
        if (value < static_cast<int64_t>(std::numeric_limits<int32_t>::min()))
            return {};
        return "#" + std::to_string(value);
    }
    if (const auto* CFP = llvm::dyn_cast<llvm::ConstantFP>(C)) {
        if (!CFP->getValueAPF().isFinite())
            return {};
        std::ostringstream os;
        os << CFP->getValueAPF().convertToDouble();
        return "#f" + os.str();
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
        case llvm::Instruction::ZExt:
        case llvm::Instruction::SExt:
        case llvm::Instruction::Trunc:
            return extractLabel(CE->getOperand(0), DL, depth + 1);
        // Relative relocations / tagged pointers: a label may sit behind a
        // constant arithmetic expression.
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
            for (const llvm::Use& U : CE->operands())
                if (const auto* operand =
                        llvm::dyn_cast<llvm::Constant>(U.get())) {
                    const std::string label =
                        extractLabel(operand, DL, depth + 1);
                    if (!label.empty())
                        return label;
                }
            return {};
        default:
            return {};
        }
    }
    return {};
}

std::string joinLabels(const std::vector<std::string>& labels,
                       bool allLabels = false) {
    std::vector<std::string> strings, numbers;
    for (const std::string& label : labels) {
        std::vector<std::string>& bucket =
            (!label.empty() && label[0] == '#') ? numbers : strings;
        if (std::find(bucket.begin(), bucket.end(), label) == bucket.end())
            bucket.push_back(label);
    }
    // By default a readable path/method beats integral padding, and integer
    // keys are used when the table has no string label. --all-labels keeps
    // both families and raises the length limits.
    std::vector<std::string> chosen;
    const size_t maxStrings = allLabels ? 8 : 4;
    for (size_t i = 0; i < strings.size() && i < maxStrings; ++i)
        chosen.push_back(strings[i]);
    if (allLabels || strings.empty())
        for (size_t i = 0; i < numbers.size() && i < (allLabels ? 4 : 4); ++i)
            chosen.push_back(numbers[i]);
    const size_t maxChars = allLabels ? 240 : 120;
    std::string out;
    for (const std::string& label : chosen) {
        if (out.size() >= maxChars)
            break;
        if (!out.empty())
            out += "|";
        out += sanitize(label);
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

/// Program-entry symbols beyond main: platform, plugin and fuzzer entry
/// points, plus user-supplied names (exact or "prefix*").
bool isFrameworkEntryName(llvm::StringRef raw,
                          const std::vector<std::string>& extra) {
    static const char* kExact[] = {
        "main", "svf.main", "wmain", "WinMain", "wWinMain", "DllMain",
        "JNI_OnLoad", "LLVMFuzzerTestOneInput", "SDL_main", "module_init"};
    for (const char* name : kExact)
        if (raw == name)
            return true;
    static const char* kPrefixes[] = {"PyInit_", "luaopen_", "napi_",
                                      "JNI_OnLoad_"};
    for (const char* prefix : kPrefixes)
        if (raw.starts_with(prefix))
            return true;
    for (const std::string& name : extra) {
        if (name.empty())
            continue;
        if (name.back() == '*') {
            if (raw.starts_with(llvm::StringRef(name).drop_back()))
                return true;
        } else if (raw == name) {
            return true;
        }
    }
    return false;
}

const llvm::Function* resolveAliaseeFunction(const llvm::Constant* C,
                                            unsigned depth = 0) {
    if (!C || depth > 6)
        return nullptr;
    if (const auto* F = llvm::dyn_cast<llvm::Function>(C))
        return F;
    if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(C))
        return resolveAliaseeFunction(alias->getAliasee(), depth + 1);
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(C))
        return GV->hasInitializer()
                   ? resolveAliaseeFunction(GV->getInitializer(), depth + 1)
                   : nullptr;
    if (const auto* CE = llvm::dyn_cast<llvm::ConstantExpr>(C))
        return CE->getNumOperands()
                   ? resolveAliaseeFunction(
                         llvm::dyn_cast<llvm::Constant>(CE->getOperand(0)),
                         depth + 1)
                   : nullptr;
    return nullptr;
}

/// A symbol is exported only when both linkage and visibility allow it.
bool isExternallyVisible(const llvm::GlobalValue& GV) {
    if (GV.hasLocalLinkage() || GV.hasAvailableExternallyLinkage())
        return false;
    return GV.getVisibility() == llvm::GlobalValue::DefaultVisibility;
}

/// Functions reachable through externally visible aliases / ifuncs. Their
/// own (possibly internal) definitions must still be reported under the name
/// the downstream analyzer can resolve.
std::unordered_set<const llvm::Function*>
collectAliasTargets(const llvm::Module& M) {
    std::unordered_set<const llvm::Function*> out;
    for (const llvm::GlobalAlias& A : M.aliases()) {
        if (!isExternallyVisible(A))
            continue;
        if (const llvm::Function* F = resolveAliaseeFunction(A.getAliasee()))
            out.insert(F);
    }
    for (const llvm::GlobalIFunc& IF : M.ifuncs()) {
        if (!isExternallyVisible(IF))
            continue;
        if (const auto* F =
                llvm::dyn_cast<llvm::Function>(IF.getResolver()))
            out.insert(F);
    }
    return out;
}

std::string metadataText(const llvm::MDNode* node) {
    if (!node)
        return {};
    for (const llvm::MDOperand& operand : node->operands())
        if (const auto* text =
                llvm::dyn_cast<llvm::MDString>(operand.get()))
            return text->getString().str();
    return {};
}

std::string functionSignature(const llvm::Function& F) {
    std::string text;
    llvm::raw_string_ostream os(text);
    F.getFunctionType()->print(os);
    os.flush();
    return text;
}

bool isMemcpyLike(const llvm::CallBase* CB) {
    const llvm::Function* callee = CB->getCalledFunction();
    if (!callee)
        return false;
    const llvm::StringRef name = callee->getName();
    return name.starts_with("llvm.memcpy") ||
           name.starts_with("llvm.memmove") || name == "memcpy" ||
           name == "memmove";
}

/// Keys under which a store to (or a load from) a pointer may be indexed: the
/// pointer itself and every enclosing GEP base, with casts stripped.
void pointerKeys(const llvm::Value* P, std::vector<const llvm::Value*>& out,
                 unsigned depth = 0) {
    if (!P || depth > 6)
        return;
    const llvm::Value* key = P->stripPointerCasts();
    if (std::find(out.begin(), out.end(), key) == out.end())
        out.push_back(key);
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(P))
        pointerKeys(gep->getPointerOperand(), out, depth + 1);
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
// Lightweight IR value flow. A pointer analysis (SVF) would be more complete,
// but these local, flow-insensitive facts cover the registration shapes that
// appear in practice without making the standalone tool depend on SVF:
//   * store / load through allocas, globals and GEPs;
//   * phi / select / insertvalue / extractvalue;
//   * a function pointer returned by another function, or forwarded by a
//     wrapper through its parameters;
//   * constant aggregates and memcpy-style table copies.
//===----------------------------------------------------------------------===//

struct ValueFlow {
    std::unordered_map<const llvm::Value*, std::vector<const llvm::Value*>>
        stores;
    std::unordered_map<const llvm::Function*,
                       std::vector<const llvm::Value*>>
        returns;
    std::unordered_map<const llvm::Argument*,
                       std::vector<const llvm::Value*>>
        args;
};

ValueFlow buildValueFlow(const llvm::Module& M) {
    ValueFlow flow;
    for (const llvm::Function& F : M) {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;
        for (const llvm::Instruction& I : llvm::instructions(F)) {
            if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                std::vector<const llvm::Value*> keys;
                pointerKeys(SI->getPointerOperand(), keys);
                for (const llvm::Value* key : keys)
                    flow.stores[key].push_back(SI->getValueOperand());
            } else if (const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
                if (RI->getReturnValue())
                    flow.returns[&F].push_back(RI->getReturnValue());
            } else if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                if (const auto* callee = llvm::dyn_cast<llvm::Function>(
                        CB->getCalledOperand()->stripPointerCasts())) {
                    const unsigned n = std::min<unsigned>(
                        CB->arg_size(),
                        static_cast<unsigned>(callee->arg_size()));
                    for (unsigned a = 0; a < n; ++a)
                        flow.args[callee->getArg(a)].push_back(
                            CB->getArgOperand(a));
                }
                if (isMemcpyLike(CB) && CB->arg_size() >= 2) {
                    std::vector<const llvm::Value*> keys;
                    pointerKeys(CB->getArgOperand(0), keys);
                    for (const llvm::Value* key : keys)
                        flow.stores[key].push_back(CB->getArgOperand(1));
                }
            }
        }
    }
    return flow;
}

void collectFunctionRefs(const llvm::Value* V, unsigned depth,
                         const ValueFlow& flow,
                         std::unordered_set<const llvm::Value*>& seen,
                         std::vector<const llvm::Function*>& out);

/// Resolve everything that may have been written through a pointer.
void collectStoredRefs(const llvm::Value* P, unsigned depth,
                       const ValueFlow& flow,
                       std::unordered_set<const llvm::Value*>& seen,
                       std::vector<const llvm::Function*>& out) {
    if (!P || depth > 8)
        return;
    std::vector<const llvm::Value*> keys;
    pointerKeys(P, keys);
    for (const llvm::Value* key : keys) {
        const auto stores = flow.stores.find(key);
        if (stores != flow.stores.end())
            for (const llvm::Value* stored : stores->second)
                collectFunctionRefs(stored, depth + 1, flow, seen, out);
        if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(key)) {
            const llvm::StringRef name = GV->getName();
            if (GV->hasInitializer() && !name.starts_with("llvm.") &&
                !isRuntimeMetadataGlobal(name)) {
                std::unordered_set<const llvm::Constant*> visiting;
                collectConstantFunctions(GV->getInitializer(), 0, visiting,
                                         out);
            }
        }
    }
}

/// Collect functions referenced by a value, following the local value flow.
void collectFunctionRefs(const llvm::Value* V, unsigned depth,
                         const ValueFlow& flow,
                         std::unordered_set<const llvm::Value*>& seen,
                         std::vector<const llvm::Function*>& out) {
    if (!V || depth > 8)
        return;
    if (const auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(V)) {
        collectFunctionRefs(alias->getAliasee(), depth + 1, flow, seen, out);
        return;
    }
    if (const auto* ifunc = llvm::dyn_cast<llvm::GlobalIFunc>(V)) {
        collectFunctionRefs(ifunc->getResolver(), depth + 1, flow, seen, out);
        return;
    }
    const llvm::Value* stripped = V->stripPointerCasts();
    if (!seen.insert(stripped).second)
        return;

    if (const auto* F = llvm::dyn_cast<llvm::Function>(stripped)) {
        out.push_back(F);
        return;
    }
    if (const auto* Arg = llvm::dyn_cast<llvm::Argument>(stripped)) {
        const auto it = flow.args.find(Arg);
        if (it != flow.args.end())
            for (const llvm::Value* actual : it->second)
                collectFunctionRefs(actual, depth + 1, flow, seen, out);
        return;
    }
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(stripped)) {
        const llvm::StringRef name = GV->getName();
        // A vtable/RTTI store must not register every virtual override.
        if (name.starts_with("llvm.") || isRuntimeMetadataGlobal(name))
            return;
        collectStoredRefs(GV, depth, flow, seen, out);
        return;
    }
    if (const auto* C = llvm::dyn_cast<llvm::Constant>(stripped)) {
        if (!llvm::isa<llvm::ConstantData>(C) &&
            !llvm::isa<llvm::ConstantAggregateZero>(C) &&
            !llvm::isa<llvm::ConstantPointerNull>(C))
            for (const llvm::Use& U : C->operands())
                if (const auto* child =
                        llvm::dyn_cast<llvm::Constant>(U.get()))
                    collectFunctionRefs(child, depth + 1, flow, seen, out);
        return;
    }

    const auto* I = llvm::dyn_cast<llvm::Instruction>(stripped);
    if (!I)
        return;
    if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(I)) {
        collectStoredRefs(LI->getPointerOperand(), depth, flow, seen, out);
        return;
    }
    if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(I)) {
        for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
            collectFunctionRefs(PN->getIncomingValue(i), depth + 1, flow,
                                seen, out);
        return;
    }
    if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(I)) {
        collectFunctionRefs(Sel->getTrueValue(), depth + 1, flow, seen, out);
        collectFunctionRefs(Sel->getFalseValue(), depth + 1, flow, seen, out);
        return;
    }
    if (const auto* CI = llvm::dyn_cast<llvm::CallInst>(I)) {
        if (const auto* callee = llvm::dyn_cast_or_null<llvm::Function>(
                CI->getCalledOperand()->stripPointerCasts())) {
            const auto it = flow.returns.find(callee);
            if (it != flow.returns.end())
                for (const llvm::Value* returned : it->second)
                    collectFunctionRefs(returned, depth + 1, flow, seen, out);
        }
        if (isMemcpyLike(CI) && CI->arg_size() >= 2)
            collectFunctionRefs(CI->getArgOperand(1), depth + 1, flow, seen,
                                out);
        return;
    }
    if (const auto* IV = llvm::dyn_cast<llvm::InsertValueInst>(I)) {
        collectFunctionRefs(IV->getAggregateOperand(), depth + 1, flow, seen,
                            out);
        collectFunctionRefs(IV->getInsertedValueOperand(), depth + 1, flow,
                            seen, out);
        return;
    }
    if (const auto* EV = llvm::dyn_cast<llvm::ExtractValueInst>(I)) {
        collectFunctionRefs(EV->getAggregateOperand(), depth + 1, flow, seen,
                            out);
        return;
    }
    if (llvm::isa<llvm::GEPOperator>(I) ||
        llvm::isa<llvm::AllocaInst>(I)) {
        collectStoredRefs(I, depth, flow, seen, out);
        if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(I))
            collectFunctionRefs(gep->getPointerOperand(), depth + 1, flow,
                                seen, out);
        return;
    }
    if (llvm::isa<llvm::CastInst>(I) || llvm::isa<llvm::FreezeInst>(I) ||
        I->getNumOperands() == 1)
        collectFunctionRefs(I->getOperand(0), depth + 1, flow, seen, out);
}

//===----------------------------------------------------------------------===//
// Constant dispatch tables + LLVM metadata globals.
//===----------------------------------------------------------------------===//

class DispatchTableScanner {
public:
    DispatchTableScanner(const llvm::Module& M, const Options& opt)
        : M_(M), DL_(M.getDataLayout()), allLabels_(opt.allLabels),
          maxDepth_(opt.allLabels ? 14u : 8u) {}

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
            if (name == "llvm.global_ctors")
                collectInitializers(GV.getInitializer());
            else if (name == "llvm.global_dtors") {
                collectInitializers(GV.getInitializer());
                // Destructor hooks are framework-triggered entries too.
                collectRetained(GV.getInitializer(), name.str());
            } else if (name == "llvm.global.annotations")
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
                       joinLabels(item.second->labels, allLabels_));
    }

    const llvm::Module& M_;
    const llvm::DataLayout& DL_;
    bool allLabels_;
    unsigned maxDepth_;
    HandlerMap handlers_;
    std::unordered_set<const llvm::Function*> initializers_;

    /// Walk a constant aggregate tree, collecting labels, handler pointers and
    /// one registration candidate per level that pairs them.
    Result visit(const llvm::Constant* C, bool labelsAllowed, unsigned depth,
                 std::unordered_set<const llvm::Constant*>& visiting) {
        Result result;
        if (!C || depth > maxDepth_)
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
    RegistrationScanner(
        const llvm::Module& M, const Options& opt,
        const std::unordered_set<const llvm::Function*>& initializers,
        const ValueFlow& flow)
        : M_(M), DL_(M.getDataLayout()), opt_(opt),
          initializers_(initializers), flow_(flow) {}

    HandlerMap run() {
        for (const llvm::Function& F : M_) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            const bool inCtor = initializers_.count(&F) != 0 ||
                                isGlobalCtorName(F.getName());
            for (const llvm::Instruction& I : llvm::instructions(F)) {
                if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                    const llvm::Function* callee = CB->getCalledFunction();
                    if (callee && callee->isIntrinsic() && !isMemcpyLike(CB))
                        continue;
                    const std::string context = describeCall(CB, inCtor);
                    for (unsigned a = 0; a < CB->arg_size(); ++a)
                        record(CB->getArgOperand(a), context);
                } else if (const auto* SI =
                               llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    if (pointsToLocalAlloca(SI->getPointerOperand()))
                        continue;  // a local fn-ptr variable is not a registry
                    record(SI->getValueOperand(), describeStore(SI, inCtor));
                } else if (const auto* RMW =
                               llvm::dyn_cast<llvm::AtomicRMWInst>(&I)) {
                    record(RMW->getValOperand(), prefix(inCtor) + "atomic");
                } else if (const auto* CX =
                               llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&I)) {
                    record(CX->getNewValOperand(), prefix(inCtor) + "atomic");
                } else if (const auto* IV =
                               llvm::dyn_cast<llvm::InsertValueInst>(&I)) {
                    record(IV->getInsertedValueOperand(),
                           prefix(inCtor) + "aggregate");
                }
            }
        }
        return handlers_;
    }

private:
    const llvm::Module& M_;
    const llvm::DataLayout& DL_;
    const Options& opt_;
    const std::unordered_set<const llvm::Function*>& initializers_;
    const ValueFlow& flow_;
    HandlerMap handlers_;

    void record(const llvm::Value* V, const std::string& context) {
        std::vector<const llvm::Function*> funcs;
        std::unordered_set<const llvm::Value*> seen;
        collectFunctionRefs(V, 0, flow_, seen, funcs);
        for (const llvm::Function* F : funcs) {
            if (F->isDeclaration() || F->isIntrinsic())
                continue;
            if (!opt_.includeLibrary) {
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
                if (!label.empty() &&
                    (opt_.allLabels || label[0] != '#'))
                    labels.push_back(label);
            }
        const std::string joined = joinLabels(labels, opt_.allLabels);
        if (!joined.empty())
            context += "(" + joined + ")";
        return context;
    }

    std::string describeStore(const llvm::StoreInst* SI, bool inCtor) const {
        std::string context = prefix(inCtor) + "store";
        const llvm::Value* ptr = SI->getPointerOperand()->stripPointerCasts();
        if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(ptr))
            context += " @" + GV->getName().str();
        const llvm::Function* fn = SI->getFunction();
        if (!fn)
            return context;
        std::string label;
        const auto inspect = [&](const llvm::Instruction& prev) {
            if (&prev == SI)
                return true;
            for (const llvm::Use& U : prev.operands())
                if (const auto* C =
                        llvm::dyn_cast<llvm::Constant>(U.get())) {
                    const std::string candidate = extractLabel(C, DL_);
                    if (!candidate.empty() &&
                        (opt_.allLabels || candidate[0] != '#'))
                        label = candidate;
                }
            return false;
        };
        if (opt_.allLabels) {
            for (const llvm::Instruction& prev : llvm::instructions(*fn))
                if (inspect(prev))
                    break;
        } else if (const llvm::BasicBlock* bb = SI->getParent()) {
            for (const llvm::Instruction& prev : *bb)
                if (inspect(prev))
                    break;
        }
        if (!label.empty())
            context += "(" + sanitize(label) + ")";
        return context;
    }
};

//===----------------------------------------------------------------------===//
// Type-erased callables and module-level metadata.
//===----------------------------------------------------------------------===//

/// Recover lambda / std::function / std::bind targets: their body never
/// appears as a function pointer, but the standard library's invoker thunk
/// calls it directly. Only invokers whose address is actually installed in a
/// callable object are considered, and terminals that user code already calls
/// directly are ignored.
class CallableScanner {
public:
    explicit CallableScanner(const llvm::Module& M) : M_(M) {}

    HandlerMap run() {
        std::unordered_set<const llvm::Function*> userCalled;
        for (const llvm::Function& F : M_) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            const std::string caller = F.getName().str();
            if (looksLikeRuntimeSymbol(caller, llvm::demangle(caller)))
                continue;
            for (const llvm::Instruction& I : llvm::instructions(F))
                if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I))
                    if (const auto* callee = llvm::dyn_cast<llvm::Function>(
                            CB->getCalledOperand()->stripPointerCasts()))
                        userCalled.insert(callee);
        }

        for (const llvm::Function& F : M_) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            const std::string raw = F.getName().str();
            if (!isInvoker(raw) || !hasAddressTakenUse(F))
                continue;
            std::unordered_set<const llvm::Function*> visited;
            std::vector<const llvm::Function*> terminals;
            walk(&F, 0, visited, terminals);
            for (const llvm::Function* target : terminals) {
                if (target->isDeclaration() || target->isIntrinsic() ||
                    userCalled.count(target))
                    continue;
                const std::string name = target->getName().str();
                if (looksLikeRuntimeSymbol(name, llvm::demangle(name)))
                    continue;
                addHandler(handlers_, target, HandlerKind::Registration,
                           "type-erased callable");
            }
        }
        return handlers_;
    }

private:
    static bool isInvoker(const std::string& raw) {
        static const char* kParts[] = {"_M_invoke", "_ZNKSt5_Bind",
                                      "_ZNKSt8_Mem_fn", "__invoke_impl",
                                      "__invoke_r"};
        for (const char* part : kParts)
            if (raw.find(part) != std::string::npos)
                return true;
        return false;
    }

    static bool hasAddressTakenUse(const llvm::Function& F) {
        for (const llvm::Use& U : F.uses()) {
            const auto* CB = llvm::dyn_cast<llvm::CallBase>(U.getUser());
            if (CB && CB->isCallee(&U))
                continue;
            return true;
        }
        return false;
    }

    void walk(const llvm::Function* F, unsigned depth,
              std::unordered_set<const llvm::Function*>& visited,
              std::vector<const llvm::Function*>& terminals) {
        if (!F || depth > 6 || !visited.insert(F).second)
            return;
        for (const llvm::Instruction& I : llvm::instructions(*F)) {
            const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!CB)
                continue;
            const auto* callee = llvm::dyn_cast_or_null<llvm::Function>(
                CB->getCalledOperand()->stripPointerCasts());
            if (!callee || callee->isDeclaration() || callee->isIntrinsic())
                continue;
            const std::string name = callee->getName().str();
            if (looksLikeRuntimeSymbol(name, llvm::demangle(name)))
                walk(callee, depth + 1, visited, terminals);
            else
                terminals.push_back(callee);
        }
    }

    const llvm::Module& M_;
    HandlerMap handlers_;
};

/// Custom sections, aliases / ifuncs, function metadata and (opt-in) vtables.
class MetadataScanner {
public:
    MetadataScanner(const llvm::Module& M, const Options& opt)
        : M_(M), opt_(opt) {}

    HandlerMap run() {
        if (opt_.sectionHandlers)
            scanSections();
        scanAliases();
        scanAnnotations();
        if (opt_.virtualHandlers)
            scanVirtualTables();
        return handlers_;
    }

private:
    void mark(const llvm::Constant* init, HandlerKind kind,
              const std::string& trigger) {
        std::vector<const llvm::Function*> funcs;
        std::unordered_set<const llvm::Constant*> visiting;
        collectConstantFunctions(init, 0, visiting, funcs);
        for (const llvm::Function* F : funcs)
            if (!F->isDeclaration() && !F->isIntrinsic())
                addHandler(handlers_, F, kind, trigger);
    }

    void scanSections() {
        for (const llvm::GlobalVariable& GV : M_.globals()) {
            const llvm::StringRef section = GV.getSection();
            const llvm::StringRef name = GV.getName();
            if (section.empty() || !GV.hasInitializer() ||
                name.starts_with("llvm.") || isRuntimeMetadataGlobal(name))
                continue;
            mark(GV.getInitializer(), HandlerKind::Section,
                 "section:" + sanitize(section.str()));
        }
    }

    void scanAliases() {
        for (const llvm::GlobalAlias& A : M_.aliases()) {
            if (!isExternallyVisible(A))
                continue;
            if (const llvm::Function* F =
                    resolveAliaseeFunction(A.getAliasee()))
                if (!F->isDeclaration() && !F->isIntrinsic())
                    addHandler(handlers_, F, HandlerKind::Alias,
                               "alias:" + A.getName().str());
        }
        for (const llvm::GlobalIFunc& IF : M_.ifuncs()) {
            if (!isExternallyVisible(IF))
                continue;
            if (const llvm::Function* F =
                    resolveAliaseeFunction(IF.getResolver()))
                addHandler(handlers_, F, HandlerKind::Alias,
                           "ifunc:" + IF.getName().str());
        }
    }

    void scanAnnotations() {
        for (const llvm::Function& F : M_) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            if (const llvm::MDNode* node = F.getMetadata("annotation")) {
                std::string text = metadataText(node);
                addHandler(handlers_, &F, HandlerKind::Annotation,
                           text.empty() ? "annotation" : text);
            }
        }
    }

    void scanVirtualTables() {
        for (const llvm::GlobalVariable& GV : M_.globals()) {
            if (!GV.hasInitializer() || !GV.getName().starts_with("_ZTV"))
                continue;
            mark(GV.getInitializer(), HandlerKind::Virtual,
                 "vtable:" + readableName(GV.getName()));
        }
    }

    const llvm::Module& M_;
    const Options& opt_;
    HandlerMap handlers_;
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

/// Nodes reachable from a real program entry (main / framework entry / a
/// global constructor). A direct call inside dead code must not prevent a
/// genuine external entry from being reported.
std::vector<char>
computeReachable(const ModuleGraph& g, const Options& opt,
                 const std::unordered_set<const llvm::Function*>& initializers) {
    std::vector<char> reachable(g.functions.size(), 0);
    std::vector<size_t> work;
    for (size_t i = 0; i < g.functions.size(); ++i) {
        const llvm::Function* F = g.functions[i];
        // A function whose address escapes may be invoked indirectly from
        // anywhere, so it is a potential caller too. Only genuinely dead code
        // (no address taken, unreachable from a program entry) is ignored.
        if (isFrameworkEntryName(F->getName(), opt.extraEntryNames) ||
            initializers.count(F) || g.addressTaken[i]) {
            reachable[i] = 1;
            work.push_back(i);
        }
    }
    while (!work.empty()) {
        const size_t u = work.back();
        work.pop_back();
        for (const int v : g.callees[u])
            if (!reachable[v]) {
                reachable[v] = 1;
                work.push_back(static_cast<size_t>(v));
            }
    }
    return reachable;
}

//===----------------------------------------------------------------------===//
// Entry selection.
//===----------------------------------------------------------------------===//

struct Entry {
    std::string name;       ///< printed name (mangled or demangled)
    std::string mangled;
    std::string demangled;
    const char* reason;
    std::string detail;     ///< trigger
    std::string file;
    unsigned start = 0;
    unsigned end = 0;
    std::string signature;
};

int reasonRank(const char* reason) {
    static const char* kOrder[] = {
        "program-entry", "route-handler", "registered-handler", "annotated",
        "retained",      "section-handler", "aliased", "virtual-handler",
        "callback",      "no-caller",      "exported", "defined"};
    for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i)
        if (std::strcmp(reason, kOrder[i]) == 0)
            return i;
    return 99;
}

std::vector<Entry> collectEntries(const llvm::Module& M, const Options& opt) {
    const ModuleGraph g = buildGraph(M);
    const ValueFlow flow = buildValueFlow(M);
    FunctionFilter filter =
        FunctionFilter::build(M, opt.namePatterns, opt.appSourceRoots);
    const bool hasDebugInfo = M.getNamedMetadata("llvm.dbg.cu") != nullptr;

    DispatchTableScanner tables(M, opt);
    HandlerMap handlers = tables.run();
    mergeHandlers(handlers,
                  RegistrationScanner(M, opt, tables.initializers(),
                                      flow).run());
    if (opt.typeErased)
        mergeHandlers(handlers, CallableScanner(M).run());
    mergeHandlers(handlers, MetadataScanner(M, opt).run());

    // Library/framework internals are not application entries. Definition-site
    // classification is only trusted when the caller supplied source roots;
    // otherwise a callback in an include/ tree or a TU built without -g would
    // be dropped just because it has no DISubprogram or lives in a header.
    for (auto it = handlers.begin(); it != handlers.end();) {
        const std::string raw = it->first->getName().str();
        const std::string demangled = llvm::demangle(raw);
        if ((!opt.includeLibrary &&
             looksLikeRuntimeSymbol(raw, demangled)) ||
            filter.isFiltered(raw, demangled))
            it = handlers.erase(it);
        else
            ++it;
    }
    const std::unordered_set<const llvm::Function*> aliasTargets =
        collectAliasTargets(M);

    // An SCC is a root group when no *reachable* direct call from outside it
    // reaches it. An escaping address still qualifies: indirect dispatch is
    // exactly how HTTP handlers and other framework callbacks are invoked.
    int sccCount = 0;
    std::vector<int> comp;
    std::vector<char> hasExternalCaller;
    std::vector<char> reachable;
    if (opt.mode == Mode::Roots) {
        comp = computeSccs(g.callees, sccCount);
        hasExternalCaller.assign(sccCount, 0);
        if (opt.reachableRoots)
            reachable = computeReachable(g, opt, tables.initializers());
        for (size_t u = 0; u < g.callees.size(); ++u) {
            if (!reachable.empty() && !reachable[u])
                continue;
            for (int v : g.callees[u])
                if (comp[u] != comp[v])
                    hasExternalCaller[comp[v]] = 1;
        }
    }

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
            if (isFrameworkEntryName(raw, opt.extraEntryNames)) {
                reason = "program-entry";
                if (raw != "main" && raw != "svf.main")
                    detail = "entry-name";
            } else if (hit != handlers.end()) {
                reason = kindReason(hit->second.kind);
                detail = hit->second.trigger;
            } else if (!hasExternalCaller[comp[i]]) {
                if (!opt.includeLibrary) {
                    if (isCompilerInitStub(raw) ||
                        tables.initializers().count(F))
                        continue;
                    const bool hasDebugForSymbol =
                        filter.findLineRange(raw) != nullptr;
                    const bool appByDebug = hasDebugInfo &&
                                           hasDebugForSymbol &&
                                           filter.isAppFunction(raw);
                    const bool excludedByDebug =
                        !opt.appSourceRoots.empty() && hasDebugInfo &&
                        hasDebugForSymbol && filter.isExcluded(raw, demangled);
                    if (filter.isFiltered(raw, demangled))
                        continue;
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
            if (isExternallyVisible(*F) || aliasTargets.count(F))
                reason = "exported";
            break;
        case Mode::All:
            reason = "defined";
            break;
        }
        if (!reason)
            continue;
        Entry entry;
        entry.name = opt.format == OutputFormat::Json
                         ? raw
                         : (opt.demangle ? demangled : raw);
        entry.mangled = raw;
        entry.demangled = demangled;
        entry.reason = reason;
        entry.detail = std::move(detail);
        entry.signature = functionSignature(*F);
        if (const FuncLineRange* range = filter.findLineRange(raw)) {
            entry.file = range->file;
            entry.start = range->start;
            entry.end = range->end;
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.name != b.name)
                      return a.name < b.name;
                  return reasonRank(a.reason) < reasonRank(b.reason);
              });
    // Demangling can collapse distinct symbols (e.g. the D0/D1 destructor
    // variants) into the same name; a plain name list only needs one of each.
    // JSON keeps every symbol so downstream callers can disambiguate.
    if (opt.format == OutputFormat::Json)
        return entries;
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

bool readPatternFile(const std::string& path,
                     std::vector<std::string>& patterns) {
    std::ifstream in(path);
    if (!in) {
        llvm::errs() << "[ERROR] cannot open filter file: " << path << "\n";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos)
            continue;
        const size_t end = line.find_last_not_of(" \t");
        line = line.substr(begin, end - begin + 1);
        if (line.empty() || line[0] == '#')
            continue;
        patterns.push_back(std::move(line));
    }
    return true;
}

/// Load every input module and link them in memory. Cross-TU dispatch tables
/// and registration sites must be visible in one module; previously only the
/// last positional argument was analysed, silently.
std::unique_ptr<llvm::Module>
loadModules(const std::vector<std::string>& files,
            llvm::LLVMContext& context) {
    std::unique_ptr<llvm::Module> merged;
    for (const std::string& file : files) {
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> part =
            llvm::parseIRFile(file, diagnostic, context);
        if (!part) {
            llvm::errs() << "[ERROR] failed to load bitcode file: " << file
                         << "\n";
            diagnostic.print("entry_extractor", llvm::errs());
            return nullptr;
        }
        if (!merged) {
            merged = std::move(part);
            continue;
        }
        if (llvm::Linker::linkModules(*merged, std::move(part))) {
            llvm::errs() << "[ERROR] failed to link bitcode file: " << file
                         << "\n";
            return nullptr;
        }
    }
    return merged;
}

/// Heuristics that tell the caller when the IR is no longer a faithful picture
/// of the source (optimisation, missing/mixed debug info, multiple modules).
std::vector<std::string> diagnoseModule(const llvm::Module& M,
                                        const Options& opt) {
    std::vector<std::string> warnings;
    size_t defined = 0, withDebug = 0, optNone = 0;
    for (const llvm::Function& F : M) {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;
        ++defined;
        if (F.getSubprogram())
            ++withDebug;
        if (F.hasOptNone())
            ++optNone;
    }
    const bool hasCompileUnit = M.getNamedMetadata("llvm.dbg.cu") != nullptr;
    if (!hasCompileUnit)
        warnings.push_back("bitcode has no debug info; source locations are "
                           "unavailable and registration tables may have been "
                           "folded by optimisation");
    else if (withDebug && withDebug < defined)
        warnings.push_back("mixed debug info: some functions have no "
                           "DISubprogram; pass --app-source-root for reliable "
                           "library classification");
    // clang -O0 emits the optnone attribute on every function; its absence in
    // a module that does carry debug info is a reliable optimisation signal.
    if (hasCompileUnit && defined > 3 && optNone == 0)
        warnings.push_back("bitcode appears optimised (-O2/LTO): constant "
                           "route tables and registration calls may have been "
                           "folded; prefer -O0 or pre-LTO bitcode");
    if (opt.bcFiles.size() > 1)
        warnings.push_back("linked " +
                           std::to_string(opt.bcFiles.size()) +
                           " bitcode modules in memory");
    return warnings;
}

void writeJson(std::ostream& out, const llvm::Module& M, const Options& opt,
               const std::vector<Entry>& entries,
               const std::vector<std::string>& warnings) {
    out << "{\n  \"module\": " << jsonEscape(M.getModuleIdentifier())
        << ",\n  \"mode\": " << jsonEscape(modeName(opt.mode))
        << ",\n  \"warnings\": [";
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i)
            out << ", ";
        out << jsonEscape(warnings[i]);
    }
    out << "],\n  \"entries\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        out << "    {\"name\": " << jsonEscape(e.name)
            << ", \"mangled\": " << jsonEscape(e.mangled)
            << ", \"demangled\": " << jsonEscape(e.demangled)
            << ", \"reason\": " << jsonEscape(e.reason)
            << ", \"trigger\": " << jsonEscape(e.detail)
            << ", \"signature\": " << jsonEscape(e.signature)
            << ", \"file\": " << jsonEscape(e.file)
            << ", \"line\": " << e.start
            << ", \"end_line\": " << e.end << "}";
        if (i + 1 < entries.size())
            out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    auto needValue = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            llvm::errs() << "[ERROR] " << flag << " needs a value\n";
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (std::strcmp(arg, "--mode") == 0) {
            const char* value = needValue(i, "--mode");
            if (!value)
                return 1;
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
        } else if (std::strcmp(arg, "--format") == 0) {
            const char* value = needValue(i, "--format");
            if (!value)
                return 1;
            if (std::strcmp(value, "names") == 0)
                opt.format = OutputFormat::Names;
            else if (std::strcmp(value, "json") == 0)
                opt.format = OutputFormat::Json;
            else {
                llvm::errs() << "[ERROR] unknown --format '" << value
                             << "' (expected names or json)\n";
                return 1;
            }
        } else if (std::strcmp(arg, "--include-library") == 0) {
            opt.includeLibrary = true;
        } else if (std::strcmp(arg, "--demangle") == 0) {
            opt.demangle = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            opt.verbose = true;
        } else if (std::strcmp(arg, "--all-labels") == 0) {
            opt.allLabels = true;
        } else if (std::strcmp(arg, "--virtual-handlers") == 0) {
            opt.virtualHandlers = true;
        } else if (std::strcmp(arg, "--no-type-erased") == 0) {
            opt.typeErased = false;
        } else if (std::strcmp(arg, "--no-section-handlers") == 0) {
            opt.sectionHandlers = false;
        } else if (std::strcmp(arg, "--legacy-roots") == 0) {
            opt.reachableRoots = false;
        } else if (std::strcmp(arg, "--output") == 0) {
            const char* value = needValue(i, "--output");
            if (!value)
                return 1;
            opt.outputFile = value;
        } else if (std::strcmp(arg, "--filter-file") == 0) {
            const char* value = needValue(i, "--filter-file");
            if (!value)
                return 1;
            opt.filterFile = value;
        } else if (std::strcmp(arg, "--app-source-root") == 0) {
            const char* value = needValue(i, "--app-source-root");
            if (!value)
                return 1;
            opt.appSourceRoots.push_back(value);
        } else if (std::strcmp(arg, "--entry-name") == 0) {
            const char* value = needValue(i, "--entry-name");
            if (!value)
                return 1;
            opt.extraEntryNames.push_back(value);
        } else if (arg[0] == '-') {
            llvm::errs() << "[ERROR] unknown option '" << arg << "'\n";
            return 1;
        } else {
            opt.bcFiles.push_back(arg);
        }
    }

    if (opt.bcFiles.empty()) {
        printUsage(argv[0]);
        return 1;
    }
    if (!opt.filterFile.empty() &&
        !readPatternFile(opt.filterFile, opt.namePatterns))
        return 1;

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = loadModules(opt.bcFiles, context);
    if (!module)
        return 1;

    const std::vector<std::string> warnings = diagnoseModule(*module, opt);
    if (opt.verbose)
        for (const std::string& warning : warnings)
            llvm::errs() << "[WARN] " << warning << "\n";

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
    if (opt.format == OutputFormat::Json) {
        writeJson(*out, *module, opt, entries, warnings);
    } else {
        for (const Entry& entry : entries) {
            *out << entry.name;
            if (opt.verbose) {
                *out << '\t' << entry.reason;
                if (!entry.detail.empty())
                    *out << '\t' << entry.detail;
            }
            *out << '\n';
        }
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
        llvm::errs() << "[INFO] " << opt.bcFiles.front() << ": " << defined
                     << " defined functions, " << entries.size()
                     << " entries (mode=" << modeName(opt.mode) << ")\n";
    }
    return 0;
}
