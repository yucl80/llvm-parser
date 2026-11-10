//===----------------------------------------------------------------------===//
// sql_extract.h — shared data model for the SQLite SQL-script extractor.
//
//   ./sql_extractor [options] <source.cpp> [more sources ...] [-- <clang args>]
//
// src/sql_extractor_source.cpp parses C/C++ sources with the libclang C API,
// resolves the SQL argument of SQLite API calls (literals, macros, constants,
// concatenations) and fills SqlReport. src/sql_extractor_cli.cpp owns the CLI
// and renders that report as JSON, text or a runnable .sql script.
//===----------------------------------------------------------------------===//

#ifndef SQL_EXTRACT_H
#define SQL_EXTRACT_H

#include <string>
#include <vector>

namespace sqlinfo {

/// A callee carrying a SQL statement in one of its arguments.
struct ApiSpec {
    std::string name; ///< simple ("sqlite3_exec") or qualified ("QSqlQuery::exec")
    int sqlArg = 0;   ///< 0-based index of the SQL argument
};

struct SqlStatement {
    std::string api;      ///< matched callee; empty for --scan-literals hits
    std::string sql;      ///< decoded, statically known SQL text
    std::string file;     ///< expansion location of the call / literal
    unsigned line = 0;
    unsigned column = 0;
    /// "call" (direct), "function" (returned by a function), "virtual"
    /// (one of several overrides), "callable" (lambda / callable object),
    /// "format" (format template), "fragment" (static part of a dynamic
    /// expression), "literal" (--scan-literals hit).
    std::string origin;
    /// Qualified producer that supplied the SQL (function/method/variable);
    /// empty when the SQL comes straight from the call argument.
    std::string symbol;
    bool resolved = true; ///< false: no static SQL text could be recovered
    bool complete = true; ///< false: template / fragment / one of many variants
};

struct SqlReport {
    std::vector<std::string> inputs;
    std::vector<ApiSpec> apis;
    std::vector<SqlStatement> statements;
    size_t unresolved = 0;  ///< API calls with a dynamic SQL argument
    size_t parseErrors = 0; ///< translation units libclang could not parse
};

struct SqlOptions {
    std::vector<std::string> inputs;
    std::vector<ApiSpec> customApis;    ///< --api name[:argno] (repeatable)
    std::string format = "json";        ///< json | text | sql
    std::string output;                 ///< empty = stdout
    bool useBuiltinApis = true;         ///< --no-builtin disables the SQLite C API
    bool scanLiterals = false;          ///< --scan-literals: SQL-looking literals too
    bool includeHeaders = false;        ///< --include-headers: non-system headers
    bool unique = false;                ///< --unique: drop duplicate SQL texts
    /// --no-dispatch: skip function-return / virtual / fragment inference
    /// and report unresolvable arguments strictly as dynamic.
    bool dispatch = true;
    std::vector<std::string> clangArgs; ///< source backend, after "--"
};

/// Built-in SQLite C API entry points (SQL is argument 1: db, sql, ...).
std::vector<ApiSpec> builtinSqliteApis();

/// Parse each input with libclang and collect SQL statements. Returns false
/// when a translation unit cannot be parsed at all, and sets \p error.
bool extractSqlFromSource(const SqlOptions& options, SqlReport& report,
                          std::string& error);

/// Render \p report in the requested format to stdout or options.output.
bool writeSqlReport(const SqlReport& report, const SqlOptions& options,
                    std::string& error);

/// Shared command-line parsing and usage, used by both backends.
bool parseSqlArgs(int argc, char** argv, SqlOptions& options,
                  std::string& error);
bool parseSqlApiSpec(const std::string& raw, ApiSpec& out, std::string& error);
void printSqlUsage(const char* prog);

} // namespace sqlinfo

#endif // SQL_EXTRACT_H
