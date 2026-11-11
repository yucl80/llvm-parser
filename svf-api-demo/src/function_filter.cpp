#include "function_filter.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <utility>

/// Strip the outermost parameter list of a demangled name:
///   "foo(int)"                                  -> "foo"
///   "operator()(int)"                           -> "operator()"
///   "(anonymous namespace)::Service::get(int)"  -> "(anonymous namespace)::Service::get"
///
/// The parameter list is the *last balanced* parenthesis group, not the first
/// '('. The Itanium demangler spells unnamed namespaces as
/// "(anonymous namespace)", and function/template parameters can contain
/// parentheses of their own ("foo(void (*)(int))"). Cutting at the first '('
/// used to erase the whole name of every function in an anonymous namespace,
/// so those spans were written with an empty function column.
std::string stripParams(std::string s) {
    const size_t close = s.rfind(')');
    if (close == std::string::npos)
        return s;  // C symbol or an undemangled name: no parameter list

    // Walk back from the final ')' to the '(' that balances it. Nested groups
    // are counted so they cannot be mistaken for the parameter list.
    int depth = 0;
    for (size_t i = close + 1; i-- > 0;) {
        if (s[i] == ')') {
            ++depth;
        } else if (s[i] == '(' && depth > 0 && --depth == 0) {
            s.erase(i);
            break;
        }
    }
    return s;
}

/// True when an Itanium-mangled symbol belongs to std:: or __gnu_cxx::.
/// The demangled name cannot answer this: template instantiations demangle with
/// a leading return type that hides the namespace. In the mangled name, after
/// "_Z" (and each "Z" of a nested local name), "N" introduces the outermost
/// scope: "St" stands for std, "9__gnu_cxx" for __gnu_cxx. Symbols not starting
/// with "_Z" (C functions, LLVM intrinsics) are never namespace members.
static bool isStdNamespaceMangled(const std::string& mangled) {
    if (mangled.rfind("_Z", 0) != 0) return false;
    size_t i = 2;
    while (i < mangled.size() && mangled[i] == 'Z') ++i;
    if (i < mangled.size() && mangled[i] == 'N') {
        ++i;
        // CV- and ref-qualifiers can precede the first nested-name component.
        while (i < mangled.size() && (mangled[i] == 'K' || mangled[i] == 'V' ||
                                      mangled[i] == 'r' || mangled[i] == 'R' ||
                                      mangled[i] == 'O'))
            ++i;
    }
    return mangled.compare(i, 2, "St") == 0 ||
           mangled.compare(i, 10, "9__gnu_cxx") == 0;
}

/// Lexically resolve ".", ".." and repeated separators in an absolute path so
/// prefix comparisons against the application's source tree are reliable
/// (compilers emit paths like "/usr/lib/gcc/.../13/../../../../include/c++/13").
/// Relative paths are returned unchanged.
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

/// True when \p file lies strictly below \p dir (directory boundary checked).
static bool isUnderDir(const std::string& file, const std::string& dir) {
    return file.size() > dir.size() &&
           file.compare(0, dir.size(), dir) == 0 &&
           file[dir.size()] == '/';
}

/// Join a DIFile's directory and filename. Clang usually emits absolute
/// filenames; the directory is prefixed only for relative ones.
static std::string joinPath(const std::string& dir, const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/') return name;
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

/// Full, normalized path of a DIFile (definition site or call site).
static std::string filePath(const llvm::DIFile* file) {
    if (!file) return {};
    return normalizePath(joinPath(file->getDirectory().str(),
                                  file->getFilename().str()));
}

/// Best-effort application directories when the caller supplied no explicit
/// roots. Do not add parent directories: a parent may contain unrelated
/// third-party or generated code. Projects with separate source/header trees
/// should pass those roots explicitly.
static std::vector<std::string> buildAppSourceDirs(const llvm::Module& M) {
    std::vector<std::string> dirs;
    for (const llvm::DICompileUnit* cu : M.debug_compile_units()) {
        const llvm::DIFile* file = cu ? cu->getFile() : nullptr;
        if (!file) continue;
        std::string source = filePath(file);
        if (source.empty()) continue;
        size_t slash = source.rfind('/');
        if (slash != std::string::npos && slash > 0)
            dirs.push_back(source.substr(0, slash));
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

static std::vector<std::string> normalizeAppSourceRoots(
    const std::vector<std::string>& roots) {
    std::vector<std::string> dirs;
    dirs.reserve(roots.size());
    for (const std::string& root : roots) {
        std::error_code ec;
        std::filesystem::path path(root);
        if (path.is_relative())
            path = std::filesystem::absolute(path, ec);
        if (ec) {
            llvm::errs() << "[ERROR] cannot resolve application source root \""
                         << root << "\": " << ec.message() << "\n";
            continue;
        }
        dirs.push_back(normalizePath(path.lexically_normal().string()));
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

/// Debug-info-independent exclusion test: std::/__gnu_cxx:: symbols (read from
/// the mangled name so return-type-prefixed template instantiations are caught),
/// the C++ ABI, unwinder, sanitizer and hardening runtime families, and the
/// allocation operators. The C library has no namespace and is classified by
/// its definition file in isExcluded(). User callbacks reached from inside
/// these functions are still reported.
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

/// Fallback when the module has no compile units: Linux system headers all
/// live under /usr/. Also used for call sites.
bool isSystemHeaderFile(const std::string& file) {
    return isUnderDir(file, "/usr");
}

/// Line range and definition file from a function's DISubprogram. The start
/// line is the signature; the end line is the largest debug line among the
/// function's own (non-inlined) instructions. hasInfo stays false when the
/// bitcode has no debug info (not built with -g).
static FuncLineRange getFuncLineRange(const llvm::Function& F) {
    FuncLineRange range;
    const llvm::DISubprogram* sp = F.getSubprogram();
    if (!sp) return range;

    range.file = sp->getFilename().str();
    range.defFile = filePath(sp->getFile());
    range.start = sp->getLine();
    range.end = range.start;

    for (const llvm::BasicBlock& bb : F) {
        for (const llvm::Instruction& inst : bb) {
            const llvm::DILocation* loc = inst.getDebugLoc();
            if (!loc || loc->getInlinedAt()) continue; // skip inlined callee code
            range.end = std::max(range.end, loc->getLine());
        }
    }
    range.hasInfo = true;
    return range;
}

FunctionFilter FunctionFilter::build(
    const llvm::Module& M, const std::vector<std::string>& namePatterns,
    const std::vector<std::string>& appSourceRoots) {
    return build(std::vector<const llvm::Module*>{&M}, namePatterns,
                 appSourceRoots);
}

FunctionFilter FunctionFilter::build(
    const std::vector<const llvm::Module*>& modules,
    const std::vector<std::string>& namePatterns,
    const std::vector<std::string>& appSourceRoots) {
    FunctionFilter filter;
    std::unordered_set<std::string> definedFunctions;
    for (const llvm::Module* module : modules) {
        if (!module) continue;
        for (const llvm::Function& F : *module) {
            const std::string name = F.getName().str();
            if (F.isDeclaration()) {
                filter.externalDecls_.insert(name);
                continue;
            }
            definedFunctions.insert(name);
            FuncLineRange range = getFuncLineRange(F);
            if (range.hasInfo)
                filter.lineRanges_[name] = std::move(range);
        }
    }
    for (const std::string& name : definedFunctions)
        filter.externalDecls_.erase(name);
    if (!appSourceRoots.empty()) {
        filter.appDirs_ = normalizeAppSourceRoots(appSourceRoots);
    } else {
        for (const llvm::Module* module : modules) {
            if (!module) continue;
            std::vector<std::string> dirs = buildAppSourceDirs(*module);
            filter.appDirs_.insert(filter.appDirs_.end(), dirs.begin(),
                                   dirs.end());
        }
    }
    std::sort(filter.appDirs_.begin(), filter.appDirs_.end());
    filter.appDirs_.erase(
        std::unique(filter.appDirs_.begin(), filter.appDirs_.end()),
        filter.appDirs_.end());

    // One ECMAScript regex per line; a malformed expression is a configuration
    // error, not a reason to abort the analysis, so report and skip it.
    filter.namePatterns_.reserve(namePatterns.size());
    for (const std::string& pattern : namePatterns) {
        try {
            filter.namePatterns_.emplace_back(pattern,
                                              std::regex::ECMAScript |
                                                  std::regex::optimize);
        } catch (const std::regex_error& e) {
            llvm::errs() << "[WARN] ignoring invalid filter pattern \"" << pattern
                         << "\": " << e.what() << "\n";
        }
    }
    return filter;
}

/// Definition-site classification shared by isExcluded() and isAppFunction():
/// true for library code, false for application code, nullopt when this symbol
/// has no debug info in the module (the caller then decides from whether the
/// module carries debug info elsewhere).
std::optional<bool> FunctionFilter::libraryByDefinition(
    const std::string& mangled) const {
    // Body-less: only a declaration in this module. The application's own code
    // is compiled into the bitcode, so a missing definition means library code.
    if (externalDecls_.count(mangled)) return true;
    auto it = lineRanges_.find(mangled);
    if (it == lineRanges_.end()) return std::nullopt;
    if (appDirs_.empty()) return isSystemHeaderFile(it->second.defFile);
    for (const std::string& dir : appDirs_)
        if (isUnderDir(it->second.defFile, dir)) return false;
    return true;
}

bool FunctionFilter::isExcluded(const std::string& mangled,
                                const std::string& demangled) const {
    if (isStdLibFunction(mangled, demangled))
        return true;
    // No debug info for this symbol: library/runtime code when the module does
    // carry debug info elsewhere; unattributable when built without -g.
    return libraryByDefinition(mangled).value_or(!lineRanges_.empty());
}

/// Positive form of the definition-site test: protects user callbacks from the
/// system-header call-site heuristic, which would otherwise hide a callback
/// invoked from out-of-line library code.
bool FunctionFilter::isAppFunction(const std::string& mangled) const {
    const std::optional<bool> library = libraryByDefinition(mangled);
    return library && !*library;
}

/// Substring regex match against the demangled signature, the parameter-
/// stripped name and the raw mangled symbol (so filters can target one
/// overload). The second search is skipped when both demangled forms coincide.
bool FunctionFilter::isFiltered(const std::string& mangled,
                                const std::string& demangled) const {
    if (namePatterns_.empty()) return false;
    const std::string base = stripParams(demangled);
    for (const std::regex& re : namePatterns_) {
        if (std::regex_search(demangled, re)) return true;
        if (base != demangled && std::regex_search(base, re)) return true;
        if (std::regex_search(mangled, re)) return true;
    }
    return false;
}

/// Line range recorded for \p mangled, or nullptr when the module has no debug
/// info for it.
const FuncLineRange* FunctionFilter::findLineRange(
    const std::string& mangled) const {
    auto it = lineRanges_.find(mangled);
    return it == lineRanges_.end() ? nullptr : &it->second;
}
