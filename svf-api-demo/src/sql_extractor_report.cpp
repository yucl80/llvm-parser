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
#include <unordered_set>
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
        "  --include-headers         Also inspect non-system headers (source only)\n"
        "  --unique                  Drop statements with duplicate SQL text\n"
        "  --no-dispatch             Report unresolvable arguments strictly as\n"
        "                            dynamic; skip function/virtual/fragment\n"
        "                            inference\n"
        "  -- <clang args...>        Pass extra flags to libclang (source only)\n"
        "  -h, --help                Show this help\n"
        "\n"
        "The numeric suffix of --api must be a decimal index in [0, 32].\n"
        "\n"
        "Examples:\n"
        "  %s --format sql --output schema.sql db.cpp\n"
        "  %s --scan-literals --format json module.bc\n"
        "  %s --api QSqlQuery::prepare:0 db.cpp -- -Iinclude -std=c++17\n",
        Prog, Prog, Prog, Prog);
}

/// "name", "name:2" and "Ns::Class::name:0" are accepted. A trailing ":..."
/// that is not a decimal index and not a "::" qualifier is rejected instead
/// of being silently treated as part of the callee name.
bool parseApiSpec(const std::string &Raw, ApiSpec &Out, std::string &Error) {
    if (Raw.empty()) {
        Error = "empty --api name";
        return false;
    }
    if (Raw.back() == ':') {
        Error = "--api missing argument index after ':'";
        return false;
    }

    std::string Name = Raw;
    int ArgNo = 0;
    const size_t Colon = Raw.rfind(':');
    if (Colon != std::string::npos) {
        const std::string Tail = Raw.substr(Colon + 1);
        bool IsNumber = true;
        for (const char C : Tail)
            if (!std::isdigit(static_cast<unsigned char>(C)))
                IsNumber = false;
        if (IsNumber) {
            long Value = 0;
            for (const char C : Tail) {
                Value = Value * 10 + static_cast<long>(C - '0');
                if (Value > 32) {
                    Error = "--api argument index out of range: " + Raw;
                    return false;
                }
            }
            Name = Raw.substr(0, Colon);
            ArgNo = static_cast<int>(Value);
        } else if (Colon == 0 || Raw[Colon - 1] != ':') {
            Error = "invalid --api argument index: " + Tail;
            return false;
        }
    }
    if (Name.empty()) {
        Error = "empty --api name";
        return false;
    }
    if (Name[0] == '-') {
        Error = "invalid --api name: " + Name;
        return false;
    }
    Out.name = Name;
    Out.sqlArg = ArgNo;
    return true;
}

/// Return the length of a valid UTF-8 sequence at \p Bytes, or 0 when the
/// bytes are not valid UTF-8 (in which case \p CodePoint is left unchanged).
size_t utf8SequenceLength(const unsigned char *Bytes, size_t Remaining,
                          unsigned &CodePoint) {
    const unsigned char Lead = Bytes[0];
    size_t Length = 0;
    unsigned Minimum = 0;
    if (Lead < 0x80) {
        CodePoint = Lead;
        return 1;
    } else if ((Lead & 0xe0) == 0xc0) {
        Length = 2;
        Minimum = 0x80;
        CodePoint = Lead & 0x1f;
    } else if ((Lead & 0xf0) == 0xe0) {
        Length = 3;
        Minimum = 0x800;
        CodePoint = Lead & 0x0f;
    } else if ((Lead & 0xf8) == 0xf0) {
        Length = 4;
        Minimum = 0x10000;
        CodePoint = Lead & 0x07;
    } else {
        return 0;
    }
    if (Remaining < Length)
        return 0;
    for (size_t I = 1; I < Length; ++I) {
        const unsigned char Continuation = Bytes[I];
        if ((Continuation & 0xc0) != 0x80)
            return 0;
        CodePoint = (CodePoint << 6) | (Continuation & 0x3f);
    }
    if (CodePoint < Minimum || CodePoint > 0x10ffff)
        return 0;
    if (CodePoint >= 0xd800 && CodePoint <= 0xdfff)
        return 0;
    return Length;
}

std::string jsonEscape(const std::string &In) {
    std::string Out;
    Out.reserve(In.size() + 8);
    for (size_t I = 0; I < In.size();) {
        const unsigned char C = static_cast<unsigned char>(In[I]);
        if (C == '"') {
            Out += "\\\"";
        } else if (C == '\\') {
            Out += "\\\\";
        } else if (C == '\n') {
            Out += "\\n";
        } else if (C == '\r') {
            Out += "\\r";
        } else if (C == '\t') {
            Out += "\\t";
        } else if (C < 0x20) {
            char Buf[8];
            std::snprintf(Buf, sizeof(Buf), "\\u%04x",
                          static_cast<unsigned>(C));
            Out += Buf;
        } else if (C < 0x80) {
            Out += static_cast<char>(C);
        } else {
            unsigned CodePoint = 0;
            const size_t Length = utf8SequenceLength(
                reinterpret_cast<const unsigned char *>(In.data() + I),
                In.size() - I, CodePoint);
            if (Length != 0) {
                Out.append(In, I, Length);
                I += Length;
                continue;
            }
            char Buf[8];
            std::snprintf(Buf, sizeof(Buf), "\\u%04x",
                          static_cast<unsigned>(C));
            Out += Buf;
        }
        ++I;
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

std::string trim(const std::string &S) {
    size_t Begin = 0, End = S.size();
    while (Begin < End &&
           std::isspace(static_cast<unsigned char>(S[Begin])))
        ++Begin;
    while (End > Begin &&
           std::isspace(static_cast<unsigned char>(S[End - 1])))
        --End;
    return S.substr(Begin, End - Begin);
}

std::string sanitizeField(const std::string &S) {
    std::string Out;
    Out.reserve(S.size());
    for (const char C : S) {
        const unsigned char U = static_cast<unsigned char>(C);
        Out += (U < 0x20 || U == 0x7f) ? ' ' : C;
    }
    return Out;
}

std::vector<std::string> sanitizeAll(const std::vector<std::string> &Items) {
    std::vector<std::string> Out;
    Out.reserve(Items.size());
    for (const std::string &Item : Items)
        Out.push_back(sanitizeField(Item));
    return Out;
}

std::string upperAscii(std::string S) {
    for (char &C : S)
        C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
    return S;
}

/// Canonical heuristic for --scan-literals, shared by both backends.
bool looksLikeSqlImpl(const std::string &Text) {
    static const char *Keywords[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP",
        "ALTER",  "PRAGMA", "WITH",   "REPLACE", "VACUUM", "ATTACH",
        "DETACH", "BEGIN",  "COMMIT", "ROLLBACK", "EXPLAIN", "ANALYZE",
        "REINDEX", "TRUNCATE", "GRANT", "REVOKE"};
    size_t I = 0;
    const size_t N = Text.size();
    for (;;) {
        while (I < N && std::isspace(static_cast<unsigned char>(Text[I])))
            ++I;
        if (I + 1 < N && Text[I] == '-' && Text[I + 1] == '-') {
            I += 2;
            while (I < N && Text[I] != '\n')
                ++I;
            continue;
        }
        if (I + 1 < N && Text[I] == '/' && Text[I + 1] == '*') {
            const size_t End = Text.find("*/", I + 2);
            I = End == std::string::npos ? N : End + 2;
            continue;
        }
        break;
    }
    const size_t Start = I;
    while (I < N && (std::isalnum(static_cast<unsigned char>(Text[I])) ||
                     Text[I] == '_'))
        ++I;
    if (I == Start)
        return false;
    const std::string Word = upperAscii(Text.substr(Start, I - Start));
    bool Known = false;
    for (const char *Keyword : Keywords)
        if (Word == Keyword) {
            Known = true;
            break;
        }
    if (!Known)
        return false;
    while (I < N && std::isspace(static_cast<unsigned char>(Text[I])))
        ++I;
    return I < N;
}

std::string dedupKey(const SqlStatement &St) {
    if (St.resolved)
        return trim(St.sql);
    // Dynamic calls have empty SQL text; deduplicate them by call site
    // instead of collapsing all of them into a single empty key.
    std::string Key = "dynamic\x1f";
    Key += St.api;
    Key += "\x1f";
    Key += St.origin;
    Key += "\x1f";
    Key += St.symbol;
    Key += "\x1f";
    Key += St.file;
    Key += "\x1f";
    Key += std::to_string(St.line);
    Key += ":";
    Key += std::to_string(St.column);
    return Key;
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
    Out += "#inputs " + joinStrings(sanitizeAll(Report.inputs)) + "\n";
    Out += "#apis " + joinStrings(sanitizeAll(apiLabels(Report))) + "\n";
    Out += "#statements " + std::to_string(Report.statements.size()) +
           " resolved " + std::to_string(countResolved(Report)) +
           " dynamic " + std::to_string(Report.unresolved) +
           " parse_errors " + std::to_string(Report.parseErrors) + "\n";
    for (const SqlStatement &St : Report.statements) {
        Out += sanitizeField(St.file) + ":" + std::to_string(St.line) + ":" +
               std::to_string(St.column) + "\t" + sanitizeField(St.origin) +
               "\t" +
               (St.api.empty() ? std::string("(literal)")
                               : sanitizeField(St.api)) +
               "\t" + stateText(St) + "\t" +
               (St.symbol.empty() ? std::string("-")
                                  : sanitizeField(St.symbol)) +
               "\t" +
               (St.resolved ? jsonEscape(St.sql) : std::string("<dynamic>")) +
               "\n";
    }
}

/// A runnable script: keep the SQL verbatim, add a provenance comment and a
/// trailing semicolon. Dynamic calls are emitted as comments so the script
/// documents what could not be extracted.
void writeSqlScript(std::string &Out, const SqlReport &Report) {
    Out += "-- Extracted SQLite SQL scripts\n-- inputs: " +
           joinStrings(sanitizeAll(Report.inputs)) + "\n";
    for (const SqlStatement &St : Report.statements) {
        Out += "\n-- " + sanitizeField(St.file) + ":" +
               std::to_string(St.line) + " " +
               (St.api.empty() ? sanitizeField(St.origin)
                               : sanitizeField(St.api)) +
               " [" + stateText(St) + "]" +
               (St.symbol.empty() ? std::string()
                                  : " via " + sanitizeField(St.symbol)) +
               "\n";
        if (!St.resolved) {
            Out += "-- SQL argument is built at runtime; not extractable\n";
            continue;
        }
        std::string Text = St.sql;
        if (!St.complete) {
            Out += "-- Partial SQL template (not executable):\n";
            size_t Begin = 0;
            while (Begin <= Text.size()) {
                const size_t End = Text.find('\n', Begin);
                Out += "-- " + Text.substr(
                                   Begin, End == std::string::npos
                                              ? std::string::npos
                                              : End - Begin) +
                       "\n";
                if (End == std::string::npos)
                    break;
                Begin = End + 1;
            }
            continue;
        }
        while (!Text.empty() &&
               std::isspace(static_cast<unsigned char>(Text.back())))
            Text.pop_back();
        if (Text.empty())
            continue;
        if (Text.back() != ';') {
            const size_t LastNewline = Text.find_last_of('\n');
            const std::string LastLine =
                LastNewline == std::string::npos
                    ? Text
                    : Text.substr(LastNewline + 1);
            // A trailing SQL line comment would swallow the semicolon.
            Text += LastLine.find("--") == std::string::npos ? ";" : "\n;";
        }
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
    Error.clear();
    const auto LooksLikeOption = [](const std::string &S) {
        return S.size() >= 2 && S[0] == '-' &&
               (S[1] == '-' || std::isalpha(static_cast<unsigned char>(S[1])));
    };

    bool AfterDashDash = false;
    for (int I = 1; I < argc; ++I) {
        const std::string Arg = argv[I];
        if (AfterDashDash) {
            Options.clangArgs.push_back(Arg);
        } else if (Arg == "--") {
            AfterDashDash = true;
        } else if (Arg == "--format" || Arg == "--output" || Arg == "--api") {
            if (I + 1 >= argc || LooksLikeOption(argv[I + 1])) {
                Error = "missing value for " + Arg;
                return false;
            }
            const std::string Value = argv[++I];
            if (Value.empty()) {
                Error = "empty value for " + Arg;
                return false;
            }
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
        } else if (Arg == "--include-headers") {
            Options.includeHeaders = true;
        } else if (Arg == "--unique") {
            Options.unique = true;
        } else if (Arg == "--no-dispatch") {
            Options.dispatch = false;
        } else if (Arg == "-h" || Arg == "--help") {
            Options.help = true;
        } else if (LooksLikeOption(Arg)) {
            Error = "unknown option: " + Arg;
            return false;
        } else {
            Options.inputs.push_back(Arg);
        }
    }
    if (Options.help)
        return true;
    if (Options.inputs.empty()) {
        Error = "no input file given";
        return false;
    }
    return true;
}

bool writeSqlReport(const SqlReport &report, const SqlOptions &options,
                    std::string &error) {
    error.clear();
    std::string Out;
    if (options.format == "json")
        writeJson(Out, report);
    else if (options.format == "text")
        writeText(Out, report);
    else if (options.format == "sql")
        writeSqlScript(Out, report);
    else {
        error = "unknown report format: " + options.format;
        return false;
    }

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
    File.flush();
    if (!File) {
        error = "cannot write output file: " + options.output;
        return false;
    }
    File.close();
    if (!File) {
        error = "cannot close output file: " + options.output;
        return false;
    }
    return true;
}

bool looksLikeSql(const std::string &text) { return looksLikeSqlImpl(text); }

void dedupSqlStatements(SqlReport &report) {
    std::unordered_set<std::string> Seen;
    std::vector<SqlStatement> Unique;
    Unique.reserve(report.statements.size());
    for (const SqlStatement &St : report.statements)
        if (Seen.insert(dedupKey(St)).second)
            Unique.push_back(St);
    report.statements = std::move(Unique);
    report.unresolved = 0;
    for (const SqlStatement &St : report.statements)
        if (!St.resolved)
            ++report.unresolved;
}

} // namespace sqlinfo
