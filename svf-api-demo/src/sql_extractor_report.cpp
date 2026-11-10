//===----------------------------------------------------------------------===//
// sql_extractor_report.cpp — shared report writers and CLI argument parsing
// SQLite SQL-script extractor (used by both the source backend and the
// bitcode backend). The backends fill sqlinfo::SqlReport; this file renders it
// as JSON (default), a line-oriented text report, or a runnable .sql script.
//===----------------------------------------------------------------------===//

#include "sql_extract.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sqlinfo;

namespace {

void printUsage(const char *Prog) {
    std::fprintf(
        stderr,
        "Usage: %s [options] <input> [more inputs ...]\n"
        "\n"
        "Extract SQLite SQL scripts from C/C++ sources or LLVM bitcode using\n"
        "libclang / the LLVM IR reader. The SQL argument of sqlite3_exec /\n"
        "sqlite3_prepare_* / ... is resolved statically (literals, constants,\n"
        "function returns, virtual overrides, callables, concatenations); runtime\n"
        "SQL is reported as dynamic or as static fragments instead of guessed.\n"
        "\n"
        "Options:\n"
        "  --format <json|text|sql>  Report layout [default: json]\n"
        "  --output <file>           Write the report to <file> [default: stdout]\n"
        "  --api <name[:argno]>      Extra SQL-carrying callee, 0-based argument\n"
        "                            index [default: 0]; repeatable, e.g.\n"
        "                            --api QSqlQuery::prepare:0 --api execQuery:0\n"
        "  --no-builtin              Do not match the built-in sqlite3_* APIs\n"
        "  --scan-literals           Also collect SQL-looking string literals\n"
        "                            not consumed by a resolvable API call\n"
        "  --unique                  Drop statements with duplicate SQL text\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Examples:\n"
        "  %s --format sql --output schema.sql db.cpp\n"
        "  %s --scan-literals --format json module.bc\n",
        Prog, Prog, Prog);
}

/// "name", "name:2" and "Ns::Class::name:0" are all accepted.
bool parseApiSpec(const std::string &Raw, ApiSpec &Out, std::string &Error) {
    std::string Name = Raw;
    int ArgNo = 0;
    const size_t Colon = Raw.rfind(':');
    if (Colon != std::string::npos && Colon + 1 < Raw.size()) {
        const std::string Tail = Raw.substr(Colon + 1);
        bool IsNumber = !Tail.empty();
        for (const char C : Tail)
            if (!std::isdigit(static_cast<unsigned char>(C)))
                IsNumber = false;
        if (IsNumber) {
            Name = Raw.substr(0, Colon);
            ArgNo = std::atoi(Tail.c_str());
        }
    }
    if (Name.empty()) {
        Error = "empty --api name";
        return false;
    }
    if (ArgNo < 0 || ArgNo > 32) {
        Error = "--api argument index out of range: " + Raw;
        return false;
    }
    Out.name = Name;
    Out.sqlArg = ArgNo;
    return true;
}

std::string jsonEscape(const std::string &In) {
    std::string Out;
    Out.reserve(In.size() + 8);
    for (const char C : In) {
        switch (C) {
        case '"': Out += "\\\""; break;
        case '\\': Out += "\\\\"; break;
        case '\n': Out += "\\n"; break;
        case '\r': Out += "\\r"; break;
        case '\t': Out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(C) < 0x20) {
                char Buf[8];
                std::snprintf(Buf, sizeof(Buf), "\\u%04x",
                              static_cast<unsigned>(C));
                Out += Buf;
            } else {
                Out += C;
            }
            break;
        }
    }
    return Out;
}

std::string joinStrings(const std::vector<std::string> &Items) {
    std::string Out;
    for (size_t I = 0; I < Items.size(); ++I) {
        if (I)
            Out += " ";
        Out += Items[I];
    }
    return Out;
}

const char *boolText(bool Value) { return Value ? "true" : "false"; }

const char *stateText(const SqlStatement &St) {
    if (!St.resolved)
        return "dynamic";
    return St.complete ? "complete" : "partial";
}

size_t countResolved(const SqlReport &Report) {
    size_t Count = 0;
    for (const SqlStatement &St : Report.statements)
        if (St.resolved)
            ++Count;
    return Count;
}

std::vector<std::string> apiLabels(const SqlReport &Report) {
    std::vector<std::string> Labels;
    for (const ApiSpec &Api : Report.apis)
        Labels.push_back(Api.name + ":" + std::to_string(Api.sqlArg));
    return Labels;
}

void writeJson(std::string &Out, const SqlReport &Report) {
    Out += "{\n  \"inputs\": [";
    for (size_t I = 0; I < Report.inputs.size(); ++I) {
        if (I)
            Out += ", ";
        Out += "\"" + jsonEscape(Report.inputs[I]) + "\"";
    }
    Out += "],\n  \"apis\": [";
    const std::vector<std::string> Labels = apiLabels(Report);
    for (size_t I = 0; I < Labels.size(); ++I) {
        if (I)
            Out += ", ";
        Out += "\"" + jsonEscape(Labels[I]) + "\"";
    }
    Out += "],\n  \"statements\": [\n";
    for (size_t I = 0; I < Report.statements.size(); ++I) {
        const SqlStatement &St = Report.statements[I];
        Out += "    {\"origin\": \"" + jsonEscape(St.origin) +
               "\", \"api\": \"" + jsonEscape(St.api) +
               "\", \"symbol\": \"" + jsonEscape(St.symbol) +
               "\", \"sql\": \"" + jsonEscape(St.sql) +
               "\", \"file\": \"" + jsonEscape(St.file) +
               "\", \"line\": " + std::to_string(St.line) +
               ", \"column\": " + std::to_string(St.column) +
               ", \"resolved\": " + boolText(St.resolved) +
               ", \"complete\": " + boolText(St.complete) + "}";
        if (I + 1 < Report.statements.size())
            Out += ",";
        Out += "\n";
    }
    Out += "  ],\n  \"statements_total\": " +
           std::to_string(Report.statements.size()) +
           ",\n  \"statements_resolved\": " +
           std::to_string(countResolved(Report)) +
           ",\n  \"parse_errors\": " + std::to_string(Report.parseErrors) +
           "\n}\n";
}

void writeText(std::string &Out, const SqlReport &Report) {
    Out += "#inputs " + joinStrings(Report.inputs) + "\n";
    Out += "#apis " + joinStrings(apiLabels(Report)) + "\n";
    Out += "#statements " + std::to_string(Report.statements.size()) +
           " resolved " + std::to_string(countResolved(Report)) +
           " dynamic " + std::to_string(Report.unresolved) +
           " parse_errors " + std::to_string(Report.parseErrors) + "\n";
    for (const SqlStatement &St : Report.statements) {
        Out += St.file + ":" + std::to_string(St.line) + ":" +
               std::to_string(St.column) + "\t" + St.origin + "\t" +
               (St.api.empty() ? std::string("(literal)") : St.api) + "\t" +
               stateText(St) + "\t" +
               (St.symbol.empty() ? std::string("-") : St.symbol) + "\t" +
               (St.resolved ? jsonEscape(St.sql) : std::string("<dynamic>")) +
               "\n";
    }
}

/// A runnable script: keep the SQL verbatim, add a provenance comment and a
/// trailing semicolon. Dynamic calls are emitted as comments so the script
/// documents what could not be extracted.
void writeSqlScript(std::string &Out, const SqlReport &Report) {
    Out += "-- Extracted SQLite SQL scripts\n-- inputs: " +
           joinStrings(Report.inputs) + "\n";
    for (const SqlStatement &St : Report.statements) {
        Out += "\n-- " + St.file + ":" + std::to_string(St.line) + " " +
               (St.api.empty() ? St.origin : St.api) + " [" +
               stateText(St) + "]" +
               (St.symbol.empty() ? std::string()
                                  : " via " + St.symbol) +
               "\n";
        if (!St.resolved) {
            Out += "-- SQL argument is built at runtime; not extractable\n";
            continue;
        }
        std::string Text = St.sql;
        while (!Text.empty() &&
               std::isspace(static_cast<unsigned char>(Text.back())))
            Text.pop_back();
        if (Text.empty())
            continue;
        if (Text.back() != ';')
            Text += ";";
        Out += Text + "\n";
    }
}

} // namespace

namespace sqlinfo {

std::vector<ApiSpec> builtinSqliteApis() {
    // sqlite3_exec / prepare* / get_table all take (db, sql, ...); the vtab
    // helper takes a CREATE TABLE statement in argument 1 as well.
    return {{"sqlite3_exec", 1},       {"sqlite3_prepare", 1},
            {"sqlite3_prepare_v2", 1}, {"sqlite3_prepare_v3", 1},
            {"sqlite3_prepare16", 1},  {"sqlite3_prepare16_v2", 1},
            {"sqlite3_prepare16_v3", 1}, {"sqlite3_get_table", 1},
            {"sqlite3_declare_vtab", 1}};
}

bool parseSqlApiSpec(const std::string &Raw, ApiSpec &Out, std::string &Error) {
    return parseApiSpec(Raw, Out, Error);
}

void printSqlUsage(const char *Prog) { printUsage(Prog); }

bool parseSqlArgs(int argc, char **argv, SqlOptions &Options,
                  std::string &Error) {
    bool AfterDashDash = false;
    for (int I = 1; I < argc; ++I) {
        const std::string Arg = argv[I];
        if (AfterDashDash) {
            Options.clangArgs.push_back(Arg);
        } else if (Arg == "--") {
            AfterDashDash = true;
        } else if (Arg == "--format" || Arg == "--output" || Arg == "--api") {
            if (I + 1 >= argc) {
                Error = "missing value for " + Arg;
                return false;
            }
            const std::string Value = argv[++I];
            if (Arg == "--format") {
                if (Value != "json" && Value != "text" && Value != "sql") {
                    Error = "unknown --format '" + Value +
                            "' (expected json, text or sql)";
                    return false;
                }
                Options.format = Value;
            } else if (Arg == "--output") {
                Options.output = Value;
            } else {
                ApiSpec Spec;
                if (!parseApiSpec(Value, Spec, Error))
                    return false;
                Options.customApis.push_back(Spec);
            }
        } else if (Arg == "--no-builtin") {
            Options.useBuiltinApis = false;
        } else if (Arg == "--scan-literals") {
            Options.scanLiterals = true;
        } else if (Arg == "--unique") {
            Options.unique = true;
        } else if (Arg == "--no-dispatch") {
            Options.dispatch = false;
        } else if (Arg == "-h" || Arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (Arg.rfind("--", 0) == 0) {
            Error = "unknown option: " + Arg;
            return false;
        } else {
            Options.inputs.push_back(Arg);
        }
    }
    if (Options.inputs.empty()) {
        Error = "no input file given";
        return false;
    }
    return true;
}

bool writeSqlReport(const SqlReport &report, const SqlOptions &options,
                    std::string &error) {
    std::string Out;
    if (options.format == "json")
        writeJson(Out, report);
    else if (options.format == "text")
        writeText(Out, report);
    else
        writeSqlScript(Out, report);

    if (options.output.empty()) {
        const bool Ok =
            std::fwrite(Out.data(), 1, Out.size(), stdout) == Out.size() &&
            std::fflush(stdout) == 0;
        if (!Ok)
            error = "cannot write report to stdout";
        return Ok;
    }
    std::ofstream File(options.output, std::ios::binary | std::ios::trunc);
    if (!File) {
        error = "cannot open output file: " + options.output;
        return false;
    }
    File << Out;
    if (!File) {
        error = "cannot write output file: " + options.output;
        return false;
    }
    return true;
}

} // namespace sqlinfo
