#!/usr/bin/env python3
"""End-to-end regression tests for sql_extractor.

Drives the built binary against the repository fixture
(test_sqlite_source.cpp) plus small generated sources. Covers:

  * built-in sqlite3 APIs and static SQL from literals, object/function-like
    macros, constants, std::string wrappers, raw strings, adjacent literals,
    parentheses and C escapes;
  * polymorphic builders: function returns, virtual overrides (all variants
    as candidates), function pointers, lambdas / std::function, format
    templates and concatenated strings;
  * SQL assembled at runtime (operator+, +=, append(), ostringstream,
    snprintf, loop accumulation) is reported as dynamic or as static
    fragments, never fabricated into a statement; --no-dispatch keeps strict
    mode;
  * --scan-literals finds unconsumed SQL and does not duplicate constants;
  * json / text / sql layouts and --output;
  * --api (including qualified names and overriding a built-in index),
    --no-builtin, --unique;
  * clang argument pass-through ("-- -D..."), multiple inputs, CLI errors.

Run through CTest (see CMakeLists.txt) or directly:
    python3 tests/sql_extractor_test.py --extractor <build>/sql_extractor \\
        --source-dir tests
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED_SQL = (
    "SELECT * FROM users WHERE name = 'bob'",
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)",
    "DROP TABLE IF EXISTS old_users",
    "INSERT INTO users (name) VALUES ('ann')",
    "CREATE INDEX idx_users_id ON users(id)",
    "CREATE INDEX idx_users_name ON users(name)",
    "SELECT id, name FROM users ORDER BY id",
    "DELETE FROM users WHERE id = 0",
    'UPDATE users SET name = "ann"\nWHERE id = 1',
    "SELECT name FROM users WHERE id = ?",
    "SELECT COUNT(*) FROM users",
)

DYNAMIC_FIXTURE = """\
typedef struct sqlite3 sqlite3;
extern "C" int sqlite3_exec(sqlite3*, const char*, void*, void*, char**);
void dup(sqlite3* db) {
  sqlite3_exec(db, "SELECT 1 FROM t", 0, 0, 0);
  sqlite3_exec(db, "SELECT 1 FROM t", 0, 0, 0);
}
"""

CONDITIONAL_FIXTURE = """\
typedef struct sqlite3 sqlite3;
extern "C" int sqlite3_exec(sqlite3*, const char*, void*, void*, char**);
void cond(sqlite3* db) {
#ifdef USE_SPECIAL
  sqlite3_exec(db, "PRAGMA special_setting = 1", 0, 0, 0);
#endif
}
"""


def run(extractor, *argv, ok=True):
    proc = subprocess.run([str(extractor), *map(str, argv)],
                          capture_output=True, text=True, timeout=120)
    if ok and proc.returncode != 0:
        raise RuntimeError("command failed (%d): %s\nstdout: %s\nstderr: %s" %
                           (proc.returncode, argv, proc.stdout, proc.stderr))
    return proc


def as_json(extractor, *argv):
    return json.loads(run(extractor, "--format", "json", *argv).stdout)


def text_rows(stdout):
    rows = []
    for line in stdout.splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 5:  # optional "via" column before the SQL column
            rows.append(parts)
    return rows


def dynamic_assembly_lines(fixture):
    """Line numbers of sqlite3_exec calls that assemble SQL at run time.

    Derived from the fixture text (the runDynamicSql function) rather than
    hard-coded, so adding a comment above it does not invalidate the suite.
    """
    lines = Path(fixture).read_text().splitlines()
    start = next(i for i, line in enumerate(lines)
                 if line.startswith("void runDynamicSql"))
    end = next((i for i in range(start + 1, len(lines)) if lines[i] == "}"),
               len(lines))
    return {i + 1 for i in range(start, end) if "sqlite3_exec(" in lines[i]}


class Suite:
    def __init__(self):
        self.current = ""
        self.passed = 0
        self.failures = []

    def section(self, name):
        self.current = name

    def check(self, condition, message):
        if condition:
            self.passed += 1
        else:
            self.failures.append("[%s] %s" % (self.current, message))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--extractor", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()
    extractor = Path(args.extractor)
    fixture = str(Path(args.source_dir) / "test_sqlite_source.cpp")
    suite = Suite()

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        suite.section("default json")
        doc = as_json(extractor, fixture)
        statements = doc["statements"]
        sqls = [s["sql"].strip() for s in statements if s["resolved"]]
        for want in EXPECTED_SQL:
            suite.check(any(want in sql for sql in sqls),
                        "missing SQL: %r" % want)
        suite.check(doc["parse_errors"] == 0, "unexpected parse errors")
        suite.check(all(s["line"] > 0 and s["file"] and s["column"] > 0
                        for s in statements),
                    "every statement needs a source location")
        dynamic = [s for s in statements if not s["resolved"]]
        dynamic_lines = {s["line"] for s in dynamic}
        assembled = dynamic_assembly_lines(fixture)
        suite.check(len(assembled) == 6,
                    "fixture should contain 6 dynamic SQL assemblies, got %d"
                    % len(assembled))
        suite.check(assembled <= dynamic_lines,
                    "every runtime-assembled SQL must be reported as dynamic")
        suite.check(dynamic_lines - assembled == {99},
                    "the only other dynamic call is the bare `table` argument")
        suite.check(all(not s["sql"] and not s["complete"] for s in dynamic),
                    "dynamic calls must carry no SQL text")
        fragments = [s for s in statements if s["origin"] == "fragment"]
        suite.check({s["sql"] for s in fragments} ==
                    {"SELECT 1", "DELETE FROM users WHERE id = 2"},
                    "conditional branches must be reported as static fragments")
        suite.check(all(not s["complete"] for s in fragments),
                    "fragments must be marked incomplete")
        suite.check(not any("SELECT 1" in sql and "DELETE" in sql
                            for sql in sqls),
                    "conditional branches must not be concatenated")
        suite.check("sqlite3_exec:1" in doc["apis"],
                    "built-in API table missing")

        suite.section("sql script")
        script = run(extractor, "--format", "sql", fixture).stdout
        suite.check("sqlite3_exec [complete]" in script,
                    "script needs provenance comments")
        suite.check("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"
                    in script, "statements need a trailing semicolon")
        suite.check("\n    ;" not in script,
                    "semicolon must follow the SQL, not trailing whitespace")
        suite.check("-- SQL argument is built at runtime" in script,
                    "dynamic calls must be documented")
        suite.check(" via SqlBuilder::sql" in script and "[partial]" in script,
                    "script must document virtual candidates")
        out_file = work / "schema.sql"
        run(extractor, "--format", "sql", "--output", str(out_file), fixture)
        suite.check(out_file.read_text() == script,
                    "--output must write exactly the stdout report")

        suite.section("polymorphic builders")
        functions = [s for s in statements if s["origin"] == "function" and
                     s["symbol"] == "schemaSql"]
        suite.check(len(functions) == 2 and
                    all(s["sql"] == "CREATE TABLE poly (id INTEGER)"
                        for s in functions),
                    "function return and function pointer resolve to schemaSql")
        table = [s for s in statements if s["origin"] == "function" and
                 s["symbol"] in ("tableSqlA", "tableSqlB")]
        suite.check({s["sql"] for s in table} ==
                    {"SELECT a FROM dispatch_table",
                     "SELECT b FROM dispatch_table"} and
                    all(not s["complete"] for s in table),
                    "function pointer table must report every alternative")
        variants = [s for s in statements if s["origin"] == "virtual"]
        suite.check({s["sql"] for s in variants} ==
                    {"SELECT base FROM poly", "SELECT name FROM users",
                     "SELECT id FROM orders", "SELECT deep FROM orders"},
                    "virtual dispatch must report base and every override")
        suite.check({s["symbol"] for s in variants} ==
                    {"SqlBuilder::sql", "UserSql::sql", "OrderSql::sql",
                     "DeepSql::sql"},
                    "virtual candidates need their qualified symbol")
        suite.check(all(not s["complete"] for s in variants),
                    "virtual candidates are alternatives, not one statement")
        callables = [s for s in statements if s["origin"] == "callable"]
        suite.check({s["sql"] for s in callables} ==
                    {"DELETE FROM poly WHERE id = 0", "SELECT 1 FROM fn"},
                    "lambda / std::function producers must be resolved")
        templates = [s for s in statements if s["origin"] == "format"]
        suite.check(len(templates) == 1 and
                    templates[0]["sql"] == "SELECT * FROM poly WHERE id = %d" and
                    not templates[0]["complete"],
                    "format template must be static text marked incomplete")
        suite.check("SELECT a FROM poly" in sqls,
                    "static concatenation must be recovered")
        strict = as_json(extractor, "--no-dispatch", fixture)
        suite.check(not any(s["origin"] in
                            ("function", "virtual", "callable", "format",
                             "fragment")
                            for s in strict["statements"]),
                    "--no-dispatch must disable producer inference")
        suite.check(sum(1 for s in strict["statements"] if not s["resolved"]) >= 7,
                    "strict mode reports unresolvable producers as dynamic")

        suite.section("custom apis")
        doc = as_json(extractor, "--api", "QSqlQuery::prepare:0",
                      "--api", "execQuery:0", fixture)
        custom = [s["sql"] for s in doc["statements"]]
        suite.check("QSqlQuery::prepare:0" in doc["apis"] and
                    "execQuery:0" in doc["apis"], "--api not registered")
        suite.check("SELECT name FROM users WHERE id = :id" in custom,
                    "qualified member API not matched")
        suite.check("VACUUM" in custom, "free-function API not matched")
        doc = as_json(extractor, "--no-builtin", "--api",
                      "Other::prepare:0", fixture)
        suite.check(not any("SELECT name FROM users WHERE id = :id" == s["sql"]
                            for s in doc["statements"]),
                    "qualified API must not match an unrelated class")
        doc = as_json(extractor, "--no-builtin", fixture)
        suite.check(doc["apis"] == [] and doc["statements"] == [],
                    "--no-builtin must disable the built-in table")
        doc = as_json(extractor, "--api", "sqlite3_exec:0", fixture)
        suite.check("sqlite3_exec:0" in doc["apis"] and
                    "sqlite3_exec:1" not in doc["apis"],
                    "--api must override a built-in argument index")

        suite.section("scan literals")
        rows = text_rows(run(extractor, "--format", "text", "--scan-literals",
                             fixture).stdout)
        literals = [r[-1] for r in rows if r[1] == "literal"]
        suite.check("PRAGMA journal_mode = WAL" in literals,
                    "unconsumed SQL literal not found")
        suite.check("SELECT 1" not in literals and
                    "DELETE FROM users WHERE id = 2" not in literals,
                    "fragments already account for conditional literals")
        suite.check("CREATE TABLE poly (id INTEGER)" not in literals,
                    "producer-returned SQL must not be duplicated")
        insert = [r for r in rows
                  if "INSERT INTO users (name) VALUES ('ann')" in r[-1]]
        suite.check(len(insert) == 1 and insert[0][1] == "call",
                    "constant must appear once, as a call")
        index = [r for r in rows if "CREATE INDEX idx_users_id" in r[-1]]
        suite.check(len(index) == 1 and index[0][1] == "call",
                    "c_str() constant must appear once, as a call")

        suite.section("unique and clang args")
        dup = work / "dup.cpp"
        dup.write_text(DYNAMIC_FIXTURE)
        suite.check(len(as_json(extractor, str(dup))["statements"]) == 2,
                    "duplicate statements should be reported by default")
        doc = as_json(extractor, "--unique", str(dup))
        suite.check(len(doc["statements"]) == 1, "--unique must de-duplicate")

        cond = work / "cond.cpp"
        cond.write_text(CONDITIONAL_FIXTURE)
        plain = as_json(extractor, str(cond))
        with_define = as_json(extractor, str(cond), "--", "-DUSE_SPECIAL")
        suite.check(not plain["statements"],
                    "conditional SQL must be hidden without the define")
        suite.check(any("PRAGMA special_setting = 1" == s["sql"]
                        for s in with_define["statements"]),
                    "clang args after -- must reach the parser")
        doc = as_json(extractor, fixture, str(cond), "--", "-DUSE_SPECIAL")
        suite.check(len(doc["inputs"]) == 2, "multiple inputs not reported")

        suite.section("cli errors")
        missing = run(extractor, str(work / "missing.cpp"), ok=False)
        suite.check(missing.returncode != 0 and "cannot parse" in missing.stderr,
                    "missing input should fail with a diagnostic")
        bad = run(extractor, "--format", "wrong", fixture, ok=False)
        suite.check(bad.returncode != 0 and "unknown --format" in bad.stderr,
                    "invalid format should be rejected")

    if suite.failures:
        for failure in suite.failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d check(s) failed" % len(suite.failures), file=sys.stderr)
        return 1
    print("sql_extractor regression: %d checks passed" % suite.passed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
