//===----------------------------------------------------------------------===//
// class_report.h — shared data model for class-information extraction.
//
// Two backends fill the very same report:
//   * class_extractor_bit_code.cpp : LLVM bitcode / IR (DWARF debug info)
//   * class_extractor_source.cpp   : C/C++ source files (libclang C API)
//
// class_extractor_cli.cpp is the single entry point: it detects the input
// file type, dispatches to the matching backend and renders the result with
// writeClassReport(). Backends never parse CLI arguments or write reports.
//===----------------------------------------------------------------------===//

#ifndef CLASS_REPORT_H
#define CLASS_REPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace classinfo {

enum class InputType { Unknown, Bitcode, Source };

struct BaseInfo {
    std::string name;       ///< qualified base class name
    std::string access;     ///< public / protected / private
    bool isVirtual = false; ///< virtual inheritance
    uint64_t offsetBits = 0; ///< DWARF only, 0 for the source backend
};

struct FieldInfo {
    std::string name;
    std::string type;
    std::string access;
    bool isStatic = false;
    uint64_t offsetBits = 0; ///< DWARF only
};

struct MethodInfo {
    std::string name;       ///< simple name ("speak", "operator()", "~Dog")
    std::string params;     ///< parameter list without parentheses
    /// "void speak(int) const"-style rendering, without static/virtual
    /// prefixes and without "= 0"/"= delete"/"= default" suffixes.
    std::string signature;
    std::string linkageName; ///< mangled symbol, bitcode backend only
    std::string access;
    std::string file;
    unsigned line = 0;
    /// source backend: ctor, dtor, template, conversion, defaulted, ...
    std::vector<std::string> flags;
    /// source backend: base-class functions this method overrides
    std::vector<std::string> overridden;
    bool isVirtual = false;
    bool isPureVirtual = false;
    bool isStatic = false;
    bool isDefinition = false;
};

struct ClassInfo {
    std::string name;  ///< qualified name
    std::string kind;  ///< class / struct / union / class template / ...
    std::string file;  ///< definition file as written by the backend
    /// Normalized absolute definition path, used to hide library classes.
    /// Empty when the backend has no absolute path (source backend).
    std::string path;
    unsigned line = 0;
    bool hasDefinition = true;
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
};

struct ClassReport {
    std::vector<std::string> inputs;
    InputType inputType = InputType::Unknown;
    /// Bitcode backend: the module carries DWARF debug info.
    bool hasDebugInfo = true;
    /// Bitcode backend: classes hidden as non-application code.
    size_t hiddenClasses = 0;
    std::vector<ClassInfo> classes;
};

struct ClassOptions {
    std::vector<std::string> inputs;
    std::string format = "json";        ///< json | text
    std::string output;                 ///< empty = stdout
    std::string inputType = "auto";     ///< auto | bitcode | source
    bool includeLibrary = false;        ///< bitcode backend
    bool allHeaders = false;            ///< source backend (--all)
    std::vector<std::string> clangArgs; ///< source backend, after "--"
};

/// Determine the backend for \p path from its extension; extension-less or
/// unusual inputs are classified by sniffing the file contents.
InputType detectInputType(const std::string& path);

const char* inputTypeName(InputType type);
const char* extractorName(InputType type);

/// Fill \p report from LLVM bitcode / textual IR debug info. Returns false on
/// a fatal input error and sets \p error.
bool extractClassesFromBitcode(const ClassOptions& options, ClassReport& report,
                               std::string& error);

/// Fill \p report from C/C++ source files using libclang. Returns false when
/// any translation unit fails to parse and sets \p error.
bool extractClassesFromSource(const ClassOptions& options, ClassReport& report,
                              std::string& error);

/// Render \p report in the requested format and write it to stdout or
/// options.output. Returns false on an I/O error and sets \p error.
bool writeClassReport(const ClassReport& report, const ClassOptions& options,
                      std::string& error);

} // namespace classinfo

#endif // CLASS_REPORT_H
