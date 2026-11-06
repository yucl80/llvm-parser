//===----------------------------------------------------------------------===//
// class_report.cpp — input type detection and the shared report writers.
//
// Both extraction backends produce a classinfo::ClassReport; the CLI renders
// it here as either JSON or text, so the output layout no longer depends on
// which backend was selected.
//===----------------------------------------------------------------------===//

#include "class_report.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_set>

namespace classinfo {
namespace {

std::string extensionOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash))
        return {};
    return path.substr(dot);
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

const char* boolText(bool value) { return value ? "true" : "false"; }

std::string join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += sep;
        out += items[i];
    }
    return out;
}

void appendStringArray(std::string& out,
                       const std::vector<std::string>& items) {
    out += "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ", ";
        out += "\"";
        out += jsonEscape(items[i]);
        out += "\"";
    }
    out += "]";
}

std::string baseClause(const BaseInfo& base) {
    std::string out = base.access;
    if (base.isVirtual) out += " virtual";
    out += " " + base.name;
    return out;
}

void reportCounts(const ClassReport& report, size_t& methods, size_t& edges,
                  size_t& overrides) {
    methods = edges = overrides = 0;
    for (const ClassInfo& cls : report.classes) {
        methods += cls.methods.size();
        edges += cls.bases.size();
        for (const MethodInfo& method : cls.methods)
            if (!method.overridden.empty()) ++overrides;
    }
}

void writeTextReport(std::string& out, const ClassReport& report) {
    size_t methodCount = 0, edgeCount = 0, overrideCount = 0;
    reportCounts(report, methodCount, edgeCount, overrideCount);

    for (const std::string& input : report.inputs)
        out += "#input " + input + "\n";
    // Keep the historical bitcode header so existing text consumers still see
    // the same "#bitcode <file>" line.
    if (report.inputType == InputType::Bitcode && !report.inputs.empty())
        out += "#bitcode " + report.inputs.front() + "\n";
    out += std::string("#input_type ") + inputTypeName(report.inputType) + "\n";
    out += std::string("#extractor ") + extractorName(report.inputType) + "\n";
    if (report.inputType == InputType::Bitcode && !report.hasDebugInfo)
        out += "#warning no_debug_info: the module carries no DWARF debug "
               "info; recompile with -g to extract classes\n";
    out += "#classes " + std::to_string(report.classes.size()) + " methods " +
           std::to_string(methodCount) + " inheritance_edges " +
           std::to_string(edgeCount) + "\n";
    if (report.hiddenClasses > 0)
        out += "#hidden_library_classes " +
               std::to_string(report.hiddenClasses) +
               " (use --include-library to keep them)\n";
    out += "\n";

    for (const ClassInfo& cls : report.classes) {
        out += cls.kind + " " + cls.name;
        if (!cls.bases.empty()) {
            out += " : ";
            for (size_t i = 0; i < cls.bases.size(); ++i) {
                if (i > 0) out += ", ";
                out += baseClause(cls.bases[i]);
            }
        }
        if (!cls.hasDefinition) out += " (declaration)";
        out += " {\n";
        out += "  # " + (cls.file.empty() ? std::string("<no file>") : cls.file) +
               ":" + std::to_string(cls.line) + "\n";

        for (const FieldInfo& field : cls.fields) {
            out += "  ";
            if (field.isStatic) out += "static ";
            out += field.type + " " + field.name + "  [" + field.access;
            if (!field.isStatic)
                out += ", offset " + std::to_string(field.offsetBits) + " bits";
            out += "]\n";
        }
        if (!cls.fields.empty() && !cls.methods.empty()) out += "\n";

        for (const MethodInfo& method : cls.methods) {
            out += "  ";
            if (method.isStatic) out += "static ";
            if (method.isVirtual) out += "virtual ";
            out += method.signature;
            if (method.isPureVirtual) out += " = 0";
            out += "  [" + method.access;
            out += method.isDefinition ? ", definition" : ", declaration";
            if (!method.linkageName.empty()) out += ", " + method.linkageName;
            if (!method.flags.empty()) out += ", " + join(method.flags, ", ");
            out += "]";
            if (!method.file.empty())
                out += "  " + method.file + ":" +
                       std::to_string(method.line);
            out += "\n";
            for (const std::string& overridden : method.overridden)
                out += "      overrides " + overridden + "\n";
        }
        out += "}\n\n";
    }

    if (!report.classes.empty()) {
        out += "[inheritance]\n";
        for (const ClassInfo& cls : report.classes) {
            for (const BaseInfo& base : cls.bases) {
                out += cls.name + " -> " + base.name + "  [" + base.access;
                if (base.isVirtual) out += ", virtual";
                out += "]\n";
            }
        }
    }

    out += "=== summary ===\n";
    out += "classes: " + std::to_string(report.classes.size()) +
           "  member functions: " + std::to_string(methodCount) +
           "  overriding functions: " +
           std::to_string(overrideCount) + "  base edges: " +
           std::to_string(edgeCount) + "\n";
}

void writeJsonReport(std::string& out, const ClassReport& report) {
    out += "{\n";
    out += "  \"input\": \"";
    if (!report.inputs.empty()) out += jsonEscape(report.inputs.front());
    out += "\",\n";
    out += "  \"inputs\": ";
    appendStringArray(out, report.inputs);
    out += ",\n";
    // Legacy alias consumed by the original bitcode-only tool.
    if (report.inputType == InputType::Bitcode) {
        out += "  \"bitcode\": \"";
        if (!report.inputs.empty()) out += jsonEscape(report.inputs.front());
        out += "\",\n";
    }
    out += "  \"input_type\": \"";
    out += inputTypeName(report.inputType);
    out += "\",\n";
    out += "  \"extractor\": \"";
    out += extractorName(report.inputType);
    out += "\",\n";
    out += "  \"debug_info\": ";
    out += boolText(report.hasDebugInfo);
    out += ",\n";
    out += "  \"hidden_library_classes\": " +
           std::to_string(report.hiddenClasses) + ",\n";
    out += "  \"classes\": [";
    for (size_t ci = 0; ci < report.classes.size(); ++ci) {
        const ClassInfo& cls = report.classes[ci];
        out += (ci == 0 ? "\n" : ",\n");
        out += "    {\n";
        out += "      \"name\": \"" + jsonEscape(cls.name) + "\",\n";
        out += "      \"kind\": \"" + jsonEscape(cls.kind) + "\",\n";
        out += "      \"file\": \"" + jsonEscape(cls.file) + "\",\n";
        out += "      \"line\": " + std::to_string(cls.line) + ",\n";
        out += "      \"definition\": ";
        out += boolText(cls.hasDefinition);
        out += ",\n";

        out += "      \"bases\": [";
        for (size_t i = 0; i < cls.bases.size(); ++i) {
            const BaseInfo& base = cls.bases[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "        {\"name\": \"" + jsonEscape(base.name) +
                   "\", \"access\": \"" + jsonEscape(base.access) +
                   "\", \"virtual\": ";
            out += boolText(base.isVirtual);
            out += ", \"offset_bits\": " + std::to_string(base.offsetBits) + "}";
        }
        if (!cls.bases.empty()) out += "\n      ";
        out += "],\n";

        out += "      \"fields\": [";
        for (size_t i = 0; i < cls.fields.size(); ++i) {
            const FieldInfo& field = cls.fields[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "        {\"name\": \"" + jsonEscape(field.name) +
                   "\", \"type\": \"" + jsonEscape(field.type) +
                   "\", \"access\": \"" + jsonEscape(field.access) +
                   "\", \"static\": ";
            out += boolText(field.isStatic);
            out += ", \"offset_bits\": " + std::to_string(field.offsetBits) +
                   "}";
        }
        if (!cls.fields.empty()) out += "\n      ";
        out += "],\n";

        out += "      \"methods\": [";
        for (size_t i = 0; i < cls.methods.size(); ++i) {
            const MethodInfo& method = cls.methods[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "        {\"name\": \"" + jsonEscape(method.name) +
                   "\", \"signature\": \"" + jsonEscape(method.signature) +
                   "\", \"linkage_name\": \"" +
                   jsonEscape(method.linkageName) + "\", \"access\": \"" +
                   jsonEscape(method.access) + "\", \"virtual\": ";
            out += boolText(method.isVirtual);
            out += ", \"pure_virtual\": ";
            out += boolText(method.isPureVirtual);
            out += ", \"static\": ";
            out += boolText(method.isStatic);
            out += ", \"definition\": ";
            out += boolText(method.isDefinition);
            out += ", \"file\": \"" + jsonEscape(method.file) +
                   "\", \"line\": " + std::to_string(method.line) +
                   ", \"flags\": ";
            appendStringArray(out, method.flags);
            out += ", \"overridden\": ";
            appendStringArray(out, method.overridden);
            out += "}";
        }
        if (!cls.methods.empty()) out += "\n      ";
        out += "]\n";
        out += "    }";
    }
    if (!report.classes.empty()) out += "\n  ";
    out += "],\n";

    // Flattened inheritance graph: one edge per (derived, base) pair.
    out += "  \"inheritance\": [";
    size_t edgeCount = 0;
    for (const ClassInfo& cls : report.classes) {
        for (const BaseInfo& base : cls.bases) {
            out += (edgeCount == 0 ? "\n" : ",\n");
            ++edgeCount;
            out += "    {\"derived\": \"" + jsonEscape(cls.name) +
                   "\", \"base\": \"" + jsonEscape(base.name) +
                   "\", \"access\": \"" + jsonEscape(base.access) +
                   "\", \"virtual\": ";
            out += boolText(base.isVirtual);
            out += ", \"offset_bits\": " + std::to_string(base.offsetBits) + "}";
        }
    }
    if (edgeCount > 0) out += "\n  ";
    out += "]\n}\n";
}

} // namespace

InputType detectInputType(const std::string& path) {
    const std::string ext = lowerAscii(extensionOf(path));
    if (ext == ".bc" || ext == ".ll")
        return InputType::Bitcode;

    static const std::unordered_set<std::string> sourceExts = {
        ".c",   ".cc",  ".cpp", ".cxx", ".c++", ".cp",  ".h",  ".hh",
        ".hpp", ".hxx", ".h++", ".inl", ".inc", ".ipp", ".tcc", ".m",
        ".mm"};
    if (sourceExts.count(ext))
        return InputType::Source;

    // Unknown extension: classify by content. Bitcode has a 4-byte magic,
    // textual LLVM IR has a recognizable header line; anything else that is
    // plain text is treated as a C/C++ source file.
    std::ifstream in(path, std::ios::binary);
    if (!in) return InputType::Unknown;
    char head[4096] = {};
    in.read(head, sizeof(head));
    const std::streamsize size = in.gcount();
    if (size <= 0) return InputType::Unknown;
    if (size >= 4 && std::memcmp(head, "BC\xc0\xde", 4) == 0)
        return InputType::Bitcode;

    const std::string text(head, static_cast<size_t>(size));
    const std::string firstLine = text.substr(0, text.find('\n'));
    if (firstLine.find("ModuleID") != std::string::npos ||
        firstLine.find("target datalayout") != std::string::npos ||
        firstLine.find("target triple") != std::string::npos)
        return InputType::Bitcode;

    for (const unsigned char c : text) {
        if (c != '\n' && c != '\r' && c != '\t' &&
            (c < 0x20 || c > 0x7e))
            return InputType::Unknown;
    }
    return InputType::Source;
}

const char* inputTypeName(InputType type) {
    switch (type) {
    case InputType::Bitcode: return "bitcode";
    case InputType::Source: return "source";
    case InputType::Unknown: break;
    }
    return "unknown";
}

const char* extractorName(InputType type) {
    switch (type) {
    case InputType::Bitcode: return "llvm-dwarf";
    case InputType::Source: return "libclang";
    case InputType::Unknown: break;
    }
    return "unknown";
}

bool writeClassReport(const ClassReport& report, const ClassOptions& options,
                      std::string& error) {
    std::string text;
    if (options.format == "json")
        writeJsonReport(text, report);
    else
        writeTextReport(text, report);

    if (options.output.empty()) {
        std::cout << text;
        std::cout.flush();
        if (!std::cout) {
            error = "failed to write the report to stdout";
            return false;
        }
        return true;
    }

    std::ofstream file(options.output, std::ios::binary);
    if (!file) {
        error = "cannot open output file: " + options.output;
        return false;
    }
    file << text;
    if (!file) {
        error = "failed to write " + options.output;
        return false;
    }
    return true;
}

} // namespace classinfo
