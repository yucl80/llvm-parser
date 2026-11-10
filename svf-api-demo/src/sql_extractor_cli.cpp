//===----------------------------------------------------------------------===//
// sql_extractor_cli.cpp — CLI driver of the SQLite SQL-script extractor
// (source backend).
//
//   ./sql_extractor [options] <source.cpp> [more sources ...] [-- <clang args>]
//
// The extraction backend (sql_extractor_source.cpp) fills sqlinfo::SqlReport;
// this file only parses the command line and renders the shared report via
// sqlinfo::writeSqlReport (implemented in sql_extractor_report.cpp).
//===----------------------------------------------------------------------===//

#include "sql_extract.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace sqlinfo;

int main(int argc, char **argv) {
    SqlOptions Options;
    std::string Error;
    if (!parseSqlArgs(argc, argv, Options, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        printSqlUsage(argv[0]);
        return 1;
    }

    SqlReport Report;
    if (!extractSqlFromSource(Options, Report, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }
    if (!writeSqlReport(Report, Options, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }

    size_t Resolved = 0;
    for (const SqlStatement &St : Report.statements)
        if (St.resolved)
            ++Resolved;
    std::fprintf(stderr,
                 "[sql_extractor] files=%zu statements=%zu resolved=%zu "
                 "dynamic=%zu\n",
                 Report.inputs.size(), Report.statements.size(), Resolved,
                 Report.unresolved);
    return 0;
}
