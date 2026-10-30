#include "function_filter.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>

/// Strip a trailing parameter list: "foo(int)" -> "foo".
std::string stripParams(std::string s) {
    size_t p = s.find('(');
    if (p != std::string::npos)
        s.erase(p);
    return s;
}

/// True when an Itanium-mangled symbol belongs to the C++ standard library or
/// a compiler runtime namespace: its outermost enclosing scope is `std` or
/// `__gnu_cxx`.
///
/// The demangled name cannot answer this reliably: function templates demangle
/// with their return type in front ("void std::__cxx11::basic_string<...>::
/// _M_construct<...>"), which hides the namespace from a leading-"std::" test.
/// The mangled name starts with the symbol's outermost scope instead: after
/// "_Z" — and each "Z" of a local name, which is nested in its enclosing
/// function's scope — a nested name "N" is followed by the first component.
/// "St" is the abbreviation for namespace std; "__gnu_cxx" is its fully
/// length-prefixed form. Anything not starting with "_Z" (C functions, LLVM
/// intrinsics) is never a C++ namespace member.
static bool isStdNamespaceMangled(const std::string& mangled) {
    if (mangled.rfind("_Z", 0) != 0) return false;
    size_t i = 2;
    while (i < mangled.size() && mangled[i] == 'Z') ++i;
    if (i < mangled.size() && mangled[i] == 'N') {
        ++i;
        // <CV-qualifiers> / <ref-qualifier> sit between "N" and the first
        // component for const/volatile/ref-qualified member functions.
        while (i < mangled.size() && (mangled[i] == 'K' || mangled[i] == 'V' ||
                                      mangled[i] == 'r' || mangled[i] == 'R' ||
                                      mangled[i] == 'O'))
            ++i;
    }
    return mangled.compare(i, 2, "St") == 0 ||
           mangled.compare(i, 10, "9__gnu_cxx") == 0;
}

/// Lexically resolve ".", ".." and repeated separators in an absolute path so
/// that prefix comparisons against the application's source tree are reliable
/// (compilers emit paths like "/usr/lib/gcc/x86_64-linux-gnu/13/../../../../
/// include/c++/13/bits/allocator.h"). Relative paths are returned unchanged.
static std::string normalizePath(const std::string& path) {
    if (path.empty() || path[0] != '/')
        return path;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (start == i) break;
        std::string part = path.substr(start, i - start);
        if (part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(std::move(part));
    }
    std::string out;
    for (const std::string& part : parts) {
        out += '/';
        out += part;
    }
    return out.empty() ? "/" : out;
}

/// Join a DIFile's directory and filename into one path. Filenames emitted by
/// clang are usually already absolute; the directory is prefixed only when the
/// name is relative.
static std::string joinPath(const std::string& dir, const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/') return name;
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

/// Parent directory of a path; empty for "/" and for a bare file name. Works
/// for relative paths too, so a compile unit recorded without an absolute
/// directory still contributes its tree.
static std::string parentDir(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return {};
    return path.substr(0, slash);
}

/// Full, normalized path of a DIFile (definition site or call site).
static std::string filePath(const llvm::DIFile* file) {
    if (!file) return {};
    return normalizePath(joinPath(file->getDirectory().str(),
                                  file->getFilename().str()));
}

/// The analyzed application's own source tree, derived from the module's
/// compile units. A function is application code when the file it is *defined*
/// in lives under one of these directories; every other definition file — the
/// libstdc++/glibc headers (gthr-default.h, bits/allocator.h), or C/C++ library
/// sources linked into the module — is library code. This decides library
/// membership from where the code comes from, instead of listing library
/// function names.
///
/// Each compile unit contributes its own directory and its parent, so headers
/// kept beside the sources (include/, ../include) remain application code.
/// Without debug info there are no compile units: the tree is empty and the
/// classifier falls back to the body/namespace checks.
static std::vector<std::string> buildAppSourceDirs(const llvm::Module& M) {
    std::vector<std::string> dirs;
    for (const llvm::DICompileUnit* cu : M.debug_compile_units()) {
        const llvm::DIFile* file = cu ? cu->getFile() : nullptr;
        if (!file) continue;
        const std::string dir = parentDir(filePath(file));
        if (dir.empty() || dir == "/") continue;
        dirs.push_back(dir);
        const std::string parent = parentDir(dir);
        if (!parent.empty() && parent != "/")
            dirs.push_back(parent);
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

/// Name-based, debug-info-independent part of the exclusion test. It covers the
/// symbols whose *namespace* identifies them as library code; everything else
/// (in particular the C library, whose functions carry no namespace) is decided
/// from the definition file by FunctionFilter::isExcluded.
///  - the std:: and __gnu_cxx:: namespaces (decided from the mangled name, so
///    return-type-prefixed template instantiations such as
///    "void std::...::_M_construct<...>" are caught too);
///  - the C++ runtime ABI and compiler runtime families (__cxa_*, __gxx_*,
///    the _Unwind_ unwinder, sanitizer hooks);
///  - the allocation operators and clang's terminate helper.
/// User callbacks called from inside these (comparators, lambdas, functors)
/// are still shown, grafted onto the caller that reached them.
static bool isStdLibFunction(const std::string& mangled,
                             const std::string& demangled) {
    if (isStdNamespaceMangled(mangled)) return true;

    static constexpr const char* kRuntimePrefixes[] = {
        "__cxa_", "__gxx_", "_Unwind_",       // C++ ABI / unwinder
        "__asan_", "__ubsan_", "__tsan_", "__msan_", "__sanitizer_",
        "__stack_chk_", "__fortify_",         // hardening hooks
    };
    for (const char* prefix : kRuntimePrefixes)
        if (demangled.rfind(prefix, 0) == 0) return true;

    if (demangled == "__clang_call_terminate") return true;

    // operator new/delete carry parameter lists ("operator new(unsigned long)");
    // strip them before the exact-name match.
    const std::string base = stripParams(demangled);
    return base == "operator new" || base == "operator new[]" ||
           base == "operator delete" || base == "operator delete[]";
}

/// Fallback for the file test when the module has no compile units: Linux
/// system include directories are all under /usr/. Used for call sites and as a
/// last resort when no application tree could be derived.
bool isSystemHeaderFile(const std::string& file) {
    if (file.empty() || file[0] != '/')
        return false;
    return file.rfind("/usr/", 0) == 0;
}

/// Compute a function's definition line range from its DISubprogram.
/// The start line is the function signature line; the end line is the
/// largest debug line among the function's own (non-inlined) instructions.
/// Returns false when the bitcode has no debug info (not built with -g).
static bool getFuncLineRange(const llvm::Function* F, FuncLineRange& range) {
    const llvm::DISubprogram* sp = F->getSubprogram();
    if (!sp) return false;

    range.file = sp->getFilename().str();
    range.defFile = filePath(sp->getFile());
    range.start = sp->getLine();
    range.end = range.start;

    for (const llvm::BasicBlock& bb : *F) {
        for (const llvm::Instruction& inst : bb) {
            const llvm::DILocation* loc = inst.getDebugLoc();
            if (!loc || loc->getInlinedAt()) continue; // skip inlined callee code
            range.end = std::max(range.end, loc->getLine());
        }
    }
    range.hasInfo = true;
    return true;
}

FunctionFilter FunctionFilter::build(const llvm::Module& M) {
    FunctionFilter filter;
    for (const llvm::Function& F : M) {
        FuncLineRange range;
        if (getFuncLineRange(&F, range))
            filter.lineRanges_[F.getName().str()] = range;
        if (F.isDeclaration())
            filter.externalDecls_.insert(F.getName().str());
    }
    filter.appDirs_ = buildAppSourceDirs(M);
    return filter;
}

bool FunctionFilter::inAppTree(const std::string& file) const {
    if (file.empty()) return false;
    for (const std::string& dir : appDirs_)
        if (file.size() > dir.size() &&
            file.compare(0, dir.size(), dir) == 0 &&
            file[dir.size()] == '/')
            return true;
    return false;
}

bool FunctionFilter::isExcluded(const std::string& mangled,
                                const std::string& demangled) const {
    if (isStdLibFunction(mangled, demangled))
        return true;
    // Body-less: only a declaration in this module. The application's own code
    // is compiled into the bitcode, so a missing definition means library code.
    if (externalDecls_.count(mangled))
        return true;
    auto it = lineRanges_.find(mangled);
    if (it != lineRanges_.end()) {
        // Defined in this module: application code iff its definition file is
        // part of the application's own sources. A definition anywhere else is
        // library code compiled into the module.
        if (appDirs_.empty())
            return isSystemHeaderFile(it->second.defFile);
        return !inAppTree(it->second.defFile);
    }
    // No debug info for this function: it is library/runtime code (libc,
    // LLVM intrinsics, RTTI), not part of the analyzed program's own sources.
    // Only exclude when the module does carry debug info elsewhere — otherwise
    // the program was built without -g and nothing is attributable.
    return !lineRanges_.empty();
}

const FuncLineRange* FunctionFilter::findLineRange(
    const std::string& mangled) const {
    auto it = lineRanges_.find(mangled);
    return it == lineRanges_.end() ? nullptr : &it->second;
}
