#include "function_filter.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <utility>

/// Remove the trailing parameter list from a demangled name:
///   "foo(int)"                                  -> "foo"
///   "operator()(int)"                           -> "operator()"
///   "(anonymous namespace)::Service::get(int)"  -> "(anonymous namespace)::Service::get"
///
/// The parameter list is the *last balanced* parenthesis group, not the first
/// '(': unnamed namespaces demangle to "(anonymous namespace)", and parameter
/// types can themselves contain parentheses, e.g. "foo(void (*)(int))".
/// Cutting at the first '(' would blank out every function in an anonymous
/// namespace.
std::string stripParams(std::string s) {
    const size_t close = s.rfind(')');
    if (close == std::string::npos)
        return s;  // C symbol or an undemangled name: no parameter list

    // Scan backwards from the final ')' to its matching '(', counting nested
    // parentheses so an inner ')' is not mistaken for the parameter list.
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

/// Detect std:: / __gnu_cxx:: membership from the mangled name.
///
/// Why not the demangled name? Template instantiations demangle with a leading
/// return type, which hides the namespace prefix.
///
/// Itanium mangling: after "_Z" (and each "Z" of a nested local name), "N"
/// introduces the outermost scope. std has three spellings there:
///   "St"                - the plain substitution
///   "9__gnu_cxx"        - the libstdc++ extension namespace
///   "Sa/Sb/Ss/Si/So/Sd" - one-letter substitutions for allocator,
///                         basic_string, string, istream, ostream, iostream
///
/// "Th"/"Tv"/"Tc" thunks are skipped so thunks to std members are recognised.
/// Symbols not starting with "_Z" (C functions, LLVM intrinsics) have no
/// namespace.
static bool isStdNamespaceMangled(const std::string& mangled) {
    if (mangled.rfind("_Z", 0) != 0) return false;
    size_t i = 2;

    // Thunks encode "_Z<kind><offset>_<target>"; skip the offset so the target
    // can be inspected with the normal rules below.
    if (i + 1 < mangled.size() && mangled[i] == 'T' &&
        (mangled[i + 1] == 'h' || mangled[i + 1] == 'v' ||
         mangled[i + 1] == 'c')) {
        i += 2;
        while (i < mangled.size() && mangled[i] != '_') ++i;
        if (i < mangled.size()) ++i;
    }

    while (i < mangled.size() && mangled[i] == 'Z') ++i;
    if (i < mangled.size() && mangled[i] == 'N') {
        ++i;
        // CV- and ref-qualifiers can precede the first nested-name component.
        while (i < mangled.size() && (mangled[i] == 'K' || mangled[i] == 'V' ||
                                      mangled[i] == 'r' || mangled[i] == 'R' ||
                                      mangled[i] == 'O'))
            ++i;
    }
    if (mangled.compare(i, 2, "St") == 0 ||
        mangled.compare(i, 10, "9__gnu_cxx") == 0)
        return true;
    // Standard substitutions are 'S' + lowercase letter; "S0_", "S1_", ...
    // are user-defined substitutions and must not match.
    return i + 1 < mangled.size() && mangled[i] == 'S' &&
           mangled[i + 1] >= 'a' && mangled[i + 1] <= 'z';
}

/// Normalize a path lexically (no filesystem access): collapse "//", drop "."
/// and resolve "..". Needed because compiler debug paths can contain ".."
/// segments, e.g. "/usr/lib/gcc/.../13/../../../../include/c++/13".
/// Relative paths retain leading ".." components; absolute paths stop at "/".
static std::string normalizePath(const std::string& path) {
    if (path.empty()) return {};
    const bool absolute = path[0] == '/';
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
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(std::move(part));
            continue;
        }
        parts.push_back(std::move(part));
    }
    std::string out = absolute ? "/" : "";
    for (const std::string& part : parts) {
        if (!out.empty() && out.back() != '/') out += '/';
        out += part;
    }
    return out.empty() ? (absolute ? "/" : ".") : out;
}

/// True if \p file is strictly inside \p dir. The trailing '/' check prevents
/// "/app-old/x" from matching "/app".
static bool isUnderDir(const std::string& file, const std::string& dir) {
    if (dir.empty()) return false;
    if (dir == "/") return file.size() > 1 && file[0] == '/';
    return file.size() > dir.size() &&
           file.compare(0, dir.size(), dir) == 0 &&
           file[dir.size()] == '/';
}

/// Combine a DIFile's directory and filename. Clang normally emits absolute
/// filenames, so the directory is only used when the filename is relative.
static std::string joinPath(const std::string& dir, const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/') return name;
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

/// Return the normalized absolute path of a DIFile (definition or call site).
/// Relative paths are made absolute and symlinks resolved so the result can be
/// compared with the normalized application roots.
///
/// Results are memoized by absolute path. A large module references the same
/// handful of system/user headers from thousands of functions and compile
/// units, and every weakly_canonical() call walks the filesystem (symlink
/// resolution + stat). The cache turns that into one lookup per unique file.
/// The returned reference stays valid for the process lifetime.
static const std::string& filePath(const llvm::DIFile* file) {
    static const std::string empty;
    if (!file) return empty;
    std::string path = joinPath(file->getDirectory().str(),
                                file->getFilename().str());
    if (path.empty()) return empty;
    if (path[0] != '/') {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(path, ec);
        if (!ec) path = p.string();
    }
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::error_code ec;
    std::filesystem::path canonical =
        std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    std::string result = normalizePath(ec ? path : canonical.string());
    return cache.emplace(std::move(path), std::move(result)).first->second;
}

/// Guess application source directories when no roots are supplied.
///
/// Include both compile-unit directories and the directories of every function
/// with a definition: inline/template functions defined in a separate include/
/// tree would otherwise look like library code.
///
/// System headers are skipped, and parent directories are deliberately not
/// added -- a parent may contain unrelated third-party or generated code.
static std::vector<std::string> buildAppSourceDirs(const llvm::Module& M) {
    std::vector<std::string> dirs;
    auto addParentDir = [&dirs](const std::string& source) {
        if (source.empty() || isSystemHeaderFile(source)) return;
        size_t slash = source.rfind('/');
        if (slash != std::string::npos && slash > 0)
            dirs.push_back(source.substr(0, slash));
    };
    for (const llvm::DICompileUnit* cu : M.debug_compile_units()) {
        const llvm::DIFile* file = cu ? cu->getFile() : nullptr;
        if (file) addParentDir(filePath(file));
    }
    for (const llvm::Function& F : M) {
        if (F.isDeclaration()) continue;
        const llvm::DISubprogram* sp = F.getSubprogram();
        if (sp) addParentDir(filePath(sp->getFile()));
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
        if (!ec) {
            std::filesystem::path canonical =
                std::filesystem::weakly_canonical(path, ec);
            if (!ec) path = canonical;
        }
        if (ec) {
            llvm::errs() << "[WARN] cannot resolve application source root \""
                         << root << "\": " << ec.message() << "; ignoring\n";
            continue;
        }
        dirs.push_back(normalizePath(path.string()));
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    if (dirs.empty())
        llvm::errs() << "[WARN] no usable application source root; falling "
                        "back to compile-unit directories\n";
    return dirs;
}

/// Recognize only the standard, replaceable global operator new/delete forms.
///
/// size_t appears as "unsigned long" (LP64) or "unsigned int" (ILP32);
/// std::align_val_t is the C++17 over-aligned variant. Any other signature,
/// e.g. "operator new(unsigned long, MyPool&)", is user code and must remain
/// visible.
static bool isStandardAllocationFunction(const std::string& demangled) {
    static const std::vector<std::string> kStandard = [] {
        std::vector<std::string> out = {"operator delete(void*)",
                                        "operator delete[](void*)"};
        for (const char* size : {"unsigned long", "unsigned int"}) {
            const std::string s(size);
            out.push_back("operator new(" + s + ")");
            out.push_back("operator new[](" + s + ")");
            out.push_back("operator new(" + s + ", std::nothrow_t const&)");
            out.push_back("operator new[](" + s + ", std::nothrow_t const&)");
            out.push_back("operator new(" + s + ", std::align_val_t)");
            out.push_back("operator new[](" + s + ", std::align_val_t)");
            out.push_back("operator new(" + s +
                          ", std::align_val_t, std::nothrow_t const&)");
            out.push_back("operator new[](" + s +
                          ", std::align_val_t, std::nothrow_t const&)");
            out.push_back("operator delete(void*, " + s + ")");
            out.push_back("operator delete[](void*, " + s + ")");
            out.push_back("operator delete(void*, std::nothrow_t const&)");
            out.push_back("operator delete[](void*, std::nothrow_t const&)");
            out.push_back("operator delete(void*, std::align_val_t)");
            out.push_back("operator delete[](void*, std::align_val_t)");
            out.push_back("operator delete(void*, " + s + ", std::align_val_t)");
            out.push_back("operator delete[](void*, " + s + ", std::align_val_t)");
            out.push_back("operator delete(void*, std::align_val_t, "
                          "std::nothrow_t const&)");
            out.push_back("operator delete[](void*, std::align_val_t, "
                          "std::nothrow_t const&)");
        }
        return out;
    }();
    return std::find(kStandard.begin(), kStandard.end(), demangled) !=
           kStandard.end();
}

/// Identify standard-library and runtime symbols by name alone (no debug info
/// required):
///   - std:: / __gnu_cxx::  (from the mangled name, see above)
///   - C++ ABI, unwinder, sanitizer and hardening runtimes
///   - standard allocation operators
///
/// The C library has no namespace; it is classified by definition file in
/// isExcluded(). User callbacks reached from these functions are still shown.
static bool isStdLibFunction(const std::string& mangled,
                             const std::string& demangled) {
    if (isStdNamespaceMangled(mangled)) return true;

    static constexpr const char* kRuntimePrefixes[] = {
        "__cxa_", "__gxx_", "_Unwind_",       // C++ ABI / unwinder
        "__asan_", "__ubsan_", "__tsan_", "__msan_", "__sanitizer_",
        "__stack_chk_", "__fortify_",         // stack protector / fortify hooks
        "__libc_", "__GI_", "__pthread_", "__emutls_", "__tls_",
        "__aeabi_", "__builtin_", "llvm.", // libc / compiler / TLS helpers
        "_GLOBAL__sub_I_", "__cxx_global_var_init",
        "__do_global_dtors_aux", "__do_global_ctors_aux",
        "register_tm_clones", "deregister_tm_clones", "frame_dummy",
        ".llvm.",                            // LLVM-generated helpers
    };
    for (const char* prefix : kRuntimePrefixes)
        if (demangled.rfind(prefix, 0) == 0 ||
            mangled.rfind(prefix, 0) == 0)
            return true;

    if (demangled == "__clang_call_terminate") return true;

    // Standard C runtime and fortified/versioned variants. Use exact names, not
    // "malloc*"/"free*" prefixes, so user code like "malloc_helper" or
    // "free_cache" is not hidden.
    static constexpr const char* kRuntimeNames[] = {
        "malloc", "calloc", "realloc", "reallocarray", "aligned_alloc",
        "memalign", "posix_memalign", "valloc", "pvalloc",
        "free", "cfree",
        "memcpy", "memmove", "memset", "memcmp", "bcopy", "bzero",
        "strlen", "strnlen", "strcpy", "strncpy", "stpcpy",
        "strcat", "strncat", "strcmp", "strncmp", "strchr", "strrchr",
        "strdup", "strndup", "strerror", "strtol", "strtoul",
        "strtoll", "strtoull", "strtof", "strtod", "strtold",
        "strtok", "strstr", "strcasestr", "strspn", "strcspn", "strpbrk",
        "strcasecmp", "strncasecmp", "memchr", "memrchr", "memccpy",
        "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
        "vsprintf", "vsnprintf", "puts", "fputs", "putchar", "fputc",
        "getchar", "fgetc", "getc", "fgets", "ungetc", "scanf", "fscanf",
        "sscanf", "vscanf", "vfscanf", "vsscanf", "fopen", "freopen",
        "fclose", "fread", "fwrite", "fseek", "ftell", "rewind", "fflush",
        "fileno", "perror", "tmpfile", "setbuf", "setvbuf",
        "abort", "exit", "_exit", "atexit", "quick_exit", "qsort", "bsearch",
        "isalnum", "isalpha", "iscntrl", "isdigit", "isgraph", "islower",
        "isprint", "ispunct", "isspace", "isupper", "isxdigit",
        "tolower", "toupper", "__ctype_b_loc", "__ctype_tolower_loc",
        "__ctype_toupper_loc",
        "__libc_malloc", "__libc_calloc", "__libc_realloc",
        "__libc_free", "__libc_cfree",
        "__malloc", "__calloc", "__realloc", "__free",
        "__memcpy_chk", "__memmove_chk", "__memset_chk",
        "__memcpy", "__memmove", "__memset",
        "__errno_location", "__libc_start_main",
        "pthread_create", "pthread_join", "pthread_exit",
        "pthread_mutex_init", "pthread_mutex_destroy",
        "pthread_mutex_lock", "pthread_mutex_trylock",
        "pthread_mutex_timedlock", "pthread_mutex_unlock",
        "pthread_rwlock_init", "pthread_rwlock_destroy",
        "pthread_rwlock_rdlock", "pthread_rwlock_tryrdlock",
        "pthread_rwlock_wrlock", "pthread_rwlock_trywrlock",
        "pthread_rwlock_timedrdlock", "pthread_rwlock_timedwrlock",
        "pthread_rwlock_unlock", "pthread_spin_init",
        "pthread_spin_destroy", "pthread_spin_lock",
        "pthread_spin_trylock", "pthread_spin_unlock",
        "dlopen", "dlclose", "dlsym", "dlvsym",
    };
    for (const char* name : kRuntimeNames)
        if (demangled == name)
            return true;

    // Linux IFUNC variants append ".sse4", ".avx2", ".generic", ".ifunc", etc.
    // Only the exact runtime names above are versioned -- "memcpy_custom"
    // stays application code.
    for (const char* name : kRuntimeNames) {
        if (demangled.compare(0, std::strlen(name), name) == 0 &&
            demangled[std::strlen(name)] == '.') {
            const std::string suffix =
                demangled.substr(std::strlen(name) + 1);
            if (suffix.rfind("sse", 0) == 0 ||
                suffix.rfind("avx", 0) == 0 ||
                suffix.rfind("generic", 0) == 0 ||
                suffix.rfind("ifunc", 0) == 0)
                return true;
        }
    }

    // Standard allocation operators only; custom pool arguments stay visible.
    return isStandardAllocationFunction(demangled);
}

/// Fallback file test: Linux system headers live under /usr/. Used when the
/// module has no compile units, and for call sites.
bool isSystemHeaderFile(const std::string& file) {
    return isUnderDir(file, "/usr");
}

/// Extract a function's source span and definition file from debug info.
///   start   - the signature line from DISubprogram
///   end     - the largest line among the function's own instructions
///             (inlined callee instructions are ignored)
///
/// hasInfo remains false when the bitcode was built without -g.
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
            if (!loc || loc->getInlinedAt()) continue;  // ignore inlined callees
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
            if (range.hasInfo) {
                // Maps are keyed by symbol name. Two modules may define
                // same-named internal symbols: keep the first and warn, rather
                // than silently overwriting it.
                if (filter.lineRanges_.count(name))
                    llvm::errs() << "[WARN] duplicate definition of \"" << name
                                 << "\" from multiple modules; using the first "
                                    "definition's location\n";
                else
                    filter.lineRanges_.emplace(name, std::move(range));
            }
        }
    }
    for (const std::string& name : definedFunctions)
        filter.externalDecls_.erase(name);
    if (!appSourceRoots.empty())
        filter.appDirs_ = normalizeAppSourceRoots(appSourceRoots);
    if (filter.appDirs_.empty()) {
        // No usable explicit roots -- fall back to the modules' debug info.
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

    // One ECMAScript regex per line. A bad pattern is a configuration error:
    // warn and skip it rather than aborting the whole analysis.
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

/// Classify a symbol by where it is defined. Shared by isExcluded() and
/// isAppFunction().
///   true     - library code
///   false    - application code
///   nullopt  - no debug info for this symbol; the caller decides
std::optional<bool> FunctionFilter::libraryByDefinition(
    const std::string& mangled) const {
    // A declaration with no IR body is imported code. Application functions
    // are compiled into the bitcode and always have a definition.
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
    // Unknown provenance stays visible. Hiding a symbol just because another
    // function in the module has debug info would drop every user function
    // from a mixed -g / non--g link. std/runtime code is still caught by the
    // name rules, imported code by the declaration test.
    return libraryByDefinition(mangled).value_or(false);
}

/// Positive counterpart of libraryByDefinition(). Keeps user callbacks visible
/// even when the call site sits in a system header. Unknown provenance is also
/// treated as application code, since a callback may come from a TU built
/// without -g.
bool FunctionFilter::isAppFunction(const std::string& mangled) const {
    const std::optional<bool> library = libraryByDefinition(mangled);
    return !library || !*library;
}

/// Regex-search the demangled signature, the name without parameters, and the
/// mangled symbol (allowing filters to select a single overload). The second
/// search is skipped when both demangled forms are identical.
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

/// Return the recorded source span for \p mangled, or nullptr if debug info is
/// unavailable.
const FuncLineRange* FunctionFilter::findLineRange(
    const std::string& mangled) const {
    auto it = lineRanges_.find(mangled);
    return it == lineRanges_.end() ? nullptr : &it->second;
}
