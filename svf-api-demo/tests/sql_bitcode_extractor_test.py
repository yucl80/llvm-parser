#!/usr/bin/env python3
"""End-to-end regression tests for the bitcode SQLite SQL-script extractor.

CMake compiles the shared test_sqlite_source.cpp fixture with -g and passes it
via --bitcode. The suite asserts SQL extraction, SVF-resolved indirect-call
candidates, dynamic values, output layouts and CLI error paths.
"""

import argparse
import json
import subprocess
import sys
import unittest
from pathlib import Path


def dynamic_assembly_lines(source_dir):
    """Line numbers of sqlite3_exec calls that assemble SQL at run time.

    Derived from the fixture text (the runDynamicSql function) rather than
    hard-coded, so adding a comment above it does not invalidate the suite.
    """
    lines = (Path(source_dir) / "test_sqlite_source.cpp").read_text().splitlines()
    start = next(i for i, line in enumerate(lines)
                 if line.startswith("void runDynamicSql"))
    end = next((i for i in range(start + 1, len(lines)) if lines[i] == "}"),
               len(lines))
    return {i + 1 for i in range(start, end) if "sqlite3_exec(" in lines[i]}


class IndirectCallTest(unittest.TestCase):
    """SVF-backed indirect-call SQL candidates on the polymorphic fixture."""
    EXTRACTOR = ""
    SOURCE_DIR = ""
    BITCODE = ""

    def run_extractor(self, *args):
        cmd = [self.EXTRACTOR, "--format", "json", *args, self.BITCODE]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        return json.loads(proc.stdout)

    def test_svf_indirect_candidates(self):
        doc = self.run_extractor()
        statements = doc["statements"]
        by_line = {}
        for st in statements:
            if st.get("resolved") and st.get("line"):
                by_line.setdefault(st["line"], []).append(st)

        def sqls_at(line):
            return {s["sql"] for s in by_line.get(line, [])}

        # Direct function-return producer (line 112) is still resolved.
        self.assertIn("CREATE TABLE poly (id INTEGER)", sqls_at(112))
        # Global function pointer `pickSql()` -> schemaSql (line 113).
        self.assertIn("CREATE TABLE poly (id INTEGER)", sqls_at(113))
        # Virtual dispatch `builder->sql()` -> UserSql::sql (line 117).
        self.assertIn("SELECT name FROM users", sqls_at(117))
        # std::function / lambda producers (lines 123, 125).
        self.assertIn("DELETE FROM poly WHERE id = 0", sqls_at(123))
        self.assertIn("SELECT 1 FROM fn", sqls_at(125))
        # Global function-pointer table `dispatchTable[0]()` (line 114):
        # both initializer targets are reported as alternative candidates.
        self.assertEqual(
            sqls_at(114),
            {"SELECT a FROM dispatch_table", "SELECT b FROM dispatch_table"},
        )
        for st in by_line[114]:
            self.assertEqual(st["origin"], "fragment")
            self.assertFalse(st["complete"])

    def test_runtime_sql_still_dynamic(self):
        doc = self.run_extractor()
        dynamic = {st.get("line") for st in doc["statements"]
                   if not st["resolved"]}
        # std::string conversions, a runtime parameter and a format template /
        # concatenation remain dynamic (never guessed).
        self.assertEqual(dynamic - dynamic_assembly_lines(self.SOURCE_DIR),
                         {77, 99, 126, 130})

    def test_dynamic_sql_assembly_is_never_guessed(self):
        # operator+, +=/to_string, append(), ostringstream, snprintf and a
        # loop-accumulated predicate all splice run-time values into SQL.
        assembled = dynamic_assembly_lines(self.SOURCE_DIR)
        self.assertEqual(len(assembled), 6)
        doc = self.run_extractor()
        by_line = {}
        for st in doc["statements"]:
            by_line.setdefault(st.get("line"), []).append(st)
        for line in assembled:
            self.assertIn(line, by_line, "missing call at line %d" % line)
            for st in by_line[line]:
                self.assertFalse(st["resolved"],
                                 "line %d must stay dynamic" % line)
                self.assertFalse(st["complete"])
                self.assertEqual(st["sql"], "",
                                 "line %d must not fabricate SQL" % line)


class SimpleTest(unittest.TestCase):
    EXTRACTOR = ""
    SOURCE_DIR = ""
    BITCODE = ""

    def run_extractor(self, *args):
        cmd = [self.EXTRACTOR, *args, self.BITCODE]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        return proc.returncode, proc.stdout, proc.stderr

    def as_json(self, *args):
        rc, out, err = self.run_extractor("--format", "json", *args)
        self.assertEqual(rc, 0, msg=err)
        return json.loads(out)

    def test_default_report(self):
        doc = self.as_json()
        self.assertEqual(doc["inputs"], [self.BITCODE])
        self.assertIn("sqlite3_exec:1", doc["apis"])
        statements = doc["statements"]
        sqls = {s["sql"] for s in statements if s["resolved"]}
        self.assertIn("SELECT * FROM users WHERE name = 'bob'", sqls)
        self.assertIn("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)",
                      sqls)
        self.assertIn("INSERT INTO users (name) VALUES ('ann')", sqls)
        self.assertIn("CREATE TABLE poly (id INTEGER)", sqls)
        self.assertIn("SELECT name FROM users", sqls)

        dynamic = [s for s in statements if not s["resolved"]]
        self.assertTrue(dynamic)
        self.assertTrue(all(not s["sql"] and not s["complete"]
                            for s in dynamic))

        fragments = [s for s in statements if s["origin"] == "fragment"]
        self.assertEqual({s["sql"] for s in fragments},
                         {"SELECT 1", "DELETE FROM users WHERE id = 2",
                          "SELECT a FROM dispatch_table",
                          "SELECT b FROM dispatch_table"})
        self.assertTrue(all(not s["complete"] for s in fragments))

    def test_provenance_present(self):
        doc = self.as_json()
        created = next(s for s in doc["statements"]
                       if s["sql"] == "CREATE TABLE poly (id INTEGER)")
        # -g gives file/line provenance on the call instructions.
        self.assertTrue(created["line"] > 0, msg=created)
        self.assertIn("test_sqlite_source.cpp", created["file"])

    def test_text_layout(self):
        rc, out, err = self.run_extractor("--format", "text")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("#statements ", out)
        self.assertIn("resolved ", out)
        self.assertIn("dynamic ", out)

    def test_sql_script_layout(self):
        rc, out, err = self.run_extractor("--format", "sql")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("INSERT INTO users (name) VALUES ('ann');", out)
        self.assertIn("via schemaSql", out)
        self.assertIn("-- SQL argument is built at runtime", out)

    def test_scan_literals_preserves_call_reports(self):
        # SQL-looking fragments used by runtime assembly are unconsumed
        # literals, so scanning adds them without changing call reports.
        original = self.as_json()
        doc = self.as_json("--scan-literals")
        scanned_calls = {
            (s["origin"], s["api"], s["symbol"], s["sql"], s["line"])
            for s in doc["statements"] if s["origin"] != "literal"
        }
        original_calls = {
            (s["origin"], s["api"], s["symbol"], s["sql"], s["line"])
            for s in original["statements"]
        }
        self.assertEqual(scanned_calls, original_calls)
        literals = {s["sql"] for s in doc["statements"]
                    if s["origin"] == "literal"}
        self.assertIn("SELECT * FROM ", literals)
        self.assertIn("SELECT name FROM %s WHERE id = %d", literals)

    def test_missing_input(self):
        proc = subprocess.run([self.EXTRACTOR, "--format", "json"],
                              capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertTrue(proc.stderr.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--extractor", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--bitcode", required=True)
    args = parser.parse_args()

    SimpleTest.EXTRACTOR = args.extractor
    SimpleTest.SOURCE_DIR = args.source_dir
    SimpleTest.BITCODE = args.bitcode

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(SimpleTest)
    IndirectCallTest.EXTRACTOR = args.extractor
    IndirectCallTest.SOURCE_DIR = args.source_dir
    IndirectCallTest.BITCODE = args.bitcode
    suite.addTests(loader.loadTestsFromTestCase(IndirectCallTest))

    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
