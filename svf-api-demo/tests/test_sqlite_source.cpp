//===----------------------------------------------------------------------===//
// test_sqlite_source.cpp — fixture for ./build/sql_extractor.
//
// Self-contained: the sqlite3 types/functions are forward-declared, so the
// source parses even on machines without the SQLite development headers.
//
//   ./svf-api-demo/build/sql_extractor --format sql \
//       svf-api-demo/tests/test_sqlite_source.cpp
//===----------------------------------------------------------------------===//

#include <functional>
#include <string>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

extern "C" int sqlite3_exec(sqlite3 *, const char *, void *, void *, char **);
extern "C" int sqlite3_prepare_v2(sqlite3 *, const char *, int,
                                  sqlite3_stmt **, const char **);
extern "C" int sqlite3_get_table(sqlite3 *, const char *, char ***, int *,
                                 int *, char **);

#define CREATE_USERS "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)"
#define RUN_SQL(db, sql) sqlite3_exec(db, sql, nullptr, nullptr, nullptr)

static const char *const kInsertUser = "INSERT INTO users (name) VALUES ('ann')";
static const char *const kPragmaOnly = "PRAGMA journal_mode = WAL";
static const std::string kCreateIndex =
    "CREATE INDEX idx_users_id ON users(id)";

// SQL produced by functions, virtual dispatch and callable objects.
static const char *schemaSql() { return "CREATE TABLE poly (id INTEGER)"; }

struct SqlBuilder {
    virtual ~SqlBuilder() = default;
    virtual const char *sql() const { return "SELECT base FROM poly"; }
};
struct UserSql : SqlBuilder {
    const char *sql() const override { return "SELECT name FROM users"; }
};
struct OrderSql : SqlBuilder {
    const char *sql() const override { return "SELECT id FROM orders"; }
};
struct DeepSql : UserSql { // chained override: Base -> UserSql -> DeepSql
    const char *sql() const override { return "SELECT deep FROM orders"; }
};

using SqlFn = const char *(*)();
static SqlFn pickSql = schemaSql;

static const char *tableSqlA() { return "SELECT a FROM dispatch_table"; }
static const char *tableSqlB() { return "SELECT b FROM dispatch_table"; }
static SqlFn dispatchTable[2] = {tableSqlA, tableSqlB};

extern std::string formatSql(const char *fmt, int id);

struct QSqlQuery {
    bool prepare(const char *sql);
    bool exec(const char *sql);
};

bool execQuery(const char *sql);

int runAll(sqlite3 *db, const char *table) {
    // Plain literal.
    sqlite3_exec(db, "SELECT * FROM users WHERE name = 'bob'", nullptr, nullptr,
                 nullptr);

    // Macro expansion (object-like and function-like).
    sqlite3_exec(db, CREATE_USERS, nullptr, nullptr, nullptr);
    RUN_SQL(db, "DROP TABLE IF EXISTS old_users");

    // Constant initialised with a literal.
    sqlite3_exec(db, kInsertUser, nullptr, nullptr, nullptr);

    // std::string constant reached through a member conversion.
    sqlite3_exec(db, kCreateIndex.c_str(), nullptr, nullptr, nullptr);

    // C++ raw string, adjacent literals, parentheses and C escapes.
    sqlite3_exec(db, R"sql(
        CREATE INDEX idx_users_name ON users(name)
    )sql",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db, "SELECT id, " "name " "FROM users ORDER BY id", nullptr,
                 nullptr, nullptr);
    sqlite3_exec(db, ("DELETE FROM users WHERE id = 0"), nullptr, nullptr,
                 nullptr);
    sqlite3_exec(db, "UPDATE users SET name = \"ann\"\nWHERE id = 1", nullptr,
                 nullptr, nullptr);

    // prepare / get_table variants.
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT name FROM users WHERE id = ?", -1, &stmt,
                       nullptr);
    sqlite3_get_table(db, "SELECT COUNT(*) FROM users", nullptr, nullptr,
                      nullptr, nullptr);

    // SQL built at runtime: reported as dynamic, never guessed.
    sqlite3_exec(db, table, nullptr, nullptr, nullptr);

    // Conditional SQL is dynamic too; --scan-literals still sees both
    // branches instead of merging them into a statement that never runs.
    sqlite3_exec(db, table ? "SELECT 1" : "DELETE FROM users WHERE id = 2",
                 nullptr, nullptr, nullptr);

    // Non-builtin callees: --api QSqlQuery::prepare:0 --api execQuery:0
    QSqlQuery query;
    query.prepare("SELECT name FROM users WHERE id = :id");
    execQuery("VACUUM");

    // A function, a function pointer and a virtual dispatch all produce SQL.
    sqlite3_exec(db, schemaSql(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, pickSql(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, dispatchTable[0](), nullptr, nullptr, nullptr);
    UserSql userSql;
    SqlBuilder *builder = &userSql;
    sqlite3_exec(db, builder->sql(), nullptr, nullptr, nullptr);

    // Lambda / std::function producers, a format template and concatenation.
    auto lambdaSql = []() -> const char * {
        return "DELETE FROM poly WHERE id = 0";
    };
    sqlite3_exec(db, lambdaSql(), nullptr, nullptr, nullptr);
    std::function<const char *()> fn = []() { return "SELECT 1 FROM fn"; };
    sqlite3_exec(db, fn(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, formatSql("SELECT * FROM poly WHERE id = %d", 7).c_str(),
                 nullptr, nullptr, nullptr);
    const std::string concat =
        std::string("SELECT ") + std::string("a FROM poly");
    sqlite3_exec(db, concat.c_str(), nullptr, nullptr, nullptr);

    return kPragmaOnly != nullptr;
}

//===----------------------------------------------------------------------===//
// Dynamic SQL assembly.
//
// Each statement below splices a value that only exists at run time (the
// `table` argument, an integer id, a loop counter) into an SQL string. The
// complete text never appears in the source, so an extractor must report it
// as dynamic / partial instead of fabricating a query that sqlite3 never
// receives. These are the classic SQL-injection shapes and deliberately sit
// next to the statically-recoverable concatenation above.
//
// Kept in a separate function *after* runAll() so the source lines of the
// cases above stay stable for the bitcode provenance assertions. The extra
// standard headers are included here, before first use, for the same reason.
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <sstream>

void runDynamicSql(sqlite3 *db, const char *table, int id) {
    // 1) operator+ : a run-time table name injected into a SELECT.
    const std::string byTable = std::string("SELECT * FROM ") + table;
    sqlite3_exec(db, byTable.c_str(), nullptr, nullptr, nullptr);

    // 2) += with std::to_string : a numeric id appended to a WHERE clause.
    // Built up step by step, so no complete SQL text exists in the source.
    std::string byId;
    byId += "DELETE FROM users WHERE id = ";
    byId += std::to_string(id);
    sqlite3_exec(db, byId.c_str(), nullptr, nullptr, nullptr);

    // 3) append() chain : several run-time fragments spliced together.
    std::string patched;
    patched.append("UPDATE users SET name = ");
    patched.append(table);
    patched.append(" WHERE id = ");
    patched.append(std::to_string(id));
    sqlite3_exec(db, patched.c_str(), nullptr, nullptr, nullptr);

    // 4) std::ostringstream : stream insertion of mixed static/dynamic parts.
    std::ostringstream os;
    os << "INSERT INTO " << table << " (id) VALUES (" << id << ")";
    sqlite3_exec(db, os.str().c_str(), nullptr, nullptr, nullptr);

    // 5) snprintf into a stack buffer : printf-style run-time substitution.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SELECT name FROM %s WHERE id = %d",
                  table, id);
    sqlite3_exec(db, buf, nullptr, nullptr, nullptr);

    // 6) loop-accumulated predicate: how many terms exist depends on `id`.
    std::string predicates;
    predicates += "SELECT id FROM poly WHERE ";
    for (int i = 0; i < id; ++i) {
        if (i)
            predicates += " OR ";
        predicates += "id = " + std::to_string(i);
    }
    sqlite3_exec(db, predicates.c_str(), nullptr, nullptr, nullptr);
}
