#ifndef FUNCTION_FILTER_H
#define FUNCTION_FILTER_H

#include <llvm/IR/Module.h>

#include <map>
#include <optional>
#include <regex>
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

/// Strip the outermost parameter list of a demangled name, e.g.
/// "foo(int)" -> "foo" and "operator()(int)" -> "operator()".
/// Prefixes such as "(anonymous namespace)::" and nested parameter
/// parentheses are preserved.
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
///
/// Independently of that classification, the user can name functions to hide
/// with a filter file (one ECMAScript regular expression per line, see
/// AnalysisConfig::nameFilters). Hidden functions — library code and filter
/// matches alike — emit no span of their own: an edge from a hidden function
/// into user code is still reported, grafted onto the nearest emitted ancestor,
/// while edges between two hidden functions are not traversed at all, so the
/// library's internal sub-call tree stays out of the report.
class FunctionFilter {
public:
    /// Classify the module's functions from their definition sites. Must run
    /// before SVF builds its model (see the class comment). \p namePatterns is
    /// the user's filter file content; an invalid expression is reported and
    /// skipped.
    static FunctionFilter build(const llvm::Module& M,
                                const std::vector<std::string>& namePatterns = {});

    /// True when \p mangled / \p demangled names library code.
    bool isExcluded(const std::string& mangled,
                    const std::string& demangled) const;

    /// True when \p mangled is defined inside the application's own source
    /// tree, i.e. genuinely user code. Keeps a callback visible even when the
    /// call site that reaches it sits in a system header.
    bool isAppFunction(const std::string& mangled) const;

    /// True when a user name pattern from the filter file matches \p mangled or
    /// \p demangled. Matching is a substring (regex) search, so "log" hides
    /// every function whose name contains it and "^log_" anchors at the start.
    bool isFiltered(const std::string& mangled,
                    const std::string& demangled) const;

    /// Line range recorded for \p mangled, or nullptr when the module has no
    /// debug info for it.
    const FuncLineRange* findLineRange(const std::string& mangled) const;

private:
    /// Definition-site classification shared by isExcluded() and
    /// isAppFunction(): true for library code, false for application code,
    /// nullopt when this symbol has no debug info in the module.
    std::optional<bool> libraryByDefinition(const std::string& mangled) const;

    std::map<std::string, FuncLineRange> lineRanges_;
    std::unordered_set<std::string> externalDecls_;
    /// Compiled user name patterns (filter file), empty when no filter file was
    /// given.
    std::vector<std::regex> namePatterns_;
    /// The analyzed application's own source tree, derived from the module's
    /// compile units: normalized, sorted, no trailing '/'.
    std::vector<std::string> appDirs_;
};

#endif // FUNCTION_FILTER_H
