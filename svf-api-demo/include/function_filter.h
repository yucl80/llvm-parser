#ifndef FUNCTION_FILTER_H
#define FUNCTION_FILTER_H

#include <llvm/IR/Module.h>

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

/// Source line range and definition file of a function, extracted from debug
/// info.
struct FuncLineRange {
    std::string file;      ///< definition file as written in the report
    std::string defFile;   ///< normalized absolute definition path (classification)
    unsigned start = 0;
    unsigned end = 0;
    bool hasInfo = false;
};

/// Strip a trailing parameter list: "foo(int)" -> "foo".
std::string stripParams(std::string s);

/// Fallback for the file test when the module has no compile units: Linux
/// system include directories are all under /usr/. Used for call sites and as a
/// last resort when no application tree could be derived.
bool isSystemHeaderFile(const std::string& file);

/// Decides whether a function is library code and therefore hidden from the
/// output call graph. Built once from the module before SVF analyses it: the
/// classification must run while SVF's ExtAPI stubs do not yet exist, or an
/// imported declaration would look like a definition.
///
/// A function is hidden when any of four signals fires, from strongest to
/// weakest:
///  1. the symbol's namespace marks it as C++ standard-library / runtime code
///     (isStdLibFunction — works with and without debug info);
///  2. the function has no IR body in this module, so it is imported code
///     (libc, libstdc++ out-of-line methods, the C++ runtime) — the
///     application's own sources are compiled into the bitcode and always have
///     a definition;
///  3. it is defined outside the application's source tree: libstdc++/glibc
///     headers such as gthr-default.h, C library sources linked into the
///     module, third-party library code. This is where the C library is
///     recognised — by the file the function is defined in, not by its name;
///  4. it has no debug info at all while the module carries debug info
///     elsewhere.
class FunctionFilter {
public:
    /// Classify the module's functions from their definition sites. Must run
    /// before SVF builds its model (see the class comment).
    static FunctionFilter build(const llvm::Module& M);

    /// True when \p mangled / \p demangled names library code.
    bool isExcluded(const std::string& mangled,
                    const std::string& demangled) const;

    /// Line range recorded for \p mangled, or nullptr when the module has no
    /// debug info for it.
    const FuncLineRange* findLineRange(const std::string& mangled) const;

private:
    /// Whether \p file lies under one of the application's source directories.
    bool inAppTree(const std::string& file) const;

    std::map<std::string, FuncLineRange> lineRanges_;
    std::unordered_set<std::string> externalDecls_;
    /// The analyzed application's own source tree, derived from the module's
    /// compile units: normalized, sorted, no trailing '/'.
    std::vector<std::string> appDirs_;
};

#endif // FUNCTION_FILTER_H
