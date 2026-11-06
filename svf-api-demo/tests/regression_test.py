#!/usr/bin/env python3
"""End-to-end regression tests for clang_analyzer.

The two test programs in this directory are only analysis inputs: nothing in
the repository asserts what the analyzer reports. This suite compiles its own
bitcode fixtures (with and without -g) and checks the CLI output, covering the
scenarios those inputs cannot verify on their own:

  * release builds without debug info (library/user classification fallback);
  * library-code exclusion and --include-stdlib;
  * filter files: demangled / parameter-stripped / mangled matches, invalid
    and missing patterns, hidden-node callback grafting;
  * multiple, mangled, missing and filtered entries, --entry-file;
  * TSV escaping, both report layouts, indirect/cycle markers;
  * newly added operator-overload and functor call scenarios;
  * anonymous-namespace function names in the report;
  * CLI error paths.

Run through CTest (see CMakeLists.txt) or directly:
    python3 tests/regression_test.py --analyzer <build>/clang_analyzer
"""

import argparse
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[2]
DEFAULT_LLVM_BIN = Path("/usr/lib/llvm-21/bin")


def tool_path(name):
    candidate = DEFAULT_LLVM_BIN / name
    if candidate.is_file():
        return str(candidate)
    return shutil.which(name) or shutil.which(name + "-21") or str(candidate)


class Suite:
    def __init__(self, args, work):
        self.args = args
        self.work = work
        self.serial = 0
        self.failures = []
        self.current = ""
        self.bc = {}
        fixtures = (
            ("prog", "test_prog.c", True, False),
            ("prog_nodbg", "test_prog.c", False, False),
            ("complex", "test_complex.cpp", True, True),
            ("complex_nodbg", "test_complex.cpp", False, True),
            ("web", "test_web_service.cpp", True, True),
        )
        for key, src, debug, cxx in fixtures:
            out = work / (key + ".bc")
            cmd = [args.clangxx if cxx else args.clang, "-O0", "-emit-llvm", "-c"]
            if debug:
                cmd.append("-g")
            if cxx:
                cmd.append("-std=c++17")
            cmd += [str(Path(args.source_dir) / src), "-o", str(out)]
            subprocess.run(cmd, check=True, capture_output=True, text=True,
                           timeout=180)
            self.bc[key] = str(out)

    def expect(self, condition, message):
        if not condition:
            self.failures.append("%s: %s" % (self.current, message))

    def run_analyzer(self, argv, fmt="tsv"):
        self.serial += 1
        out = self.work / ("report%d.%s" % (self.serial, fmt))
        cmd = [self.args.analyzer, "--format", fmt, "--output", str(out)]
        cmd += [str(a) for a in argv]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        return SimpleNamespace(
            rc=proc.returncode,
            text=out.read_text(encoding="utf-8", errors="replace")
            if out.exists() else "",
            log=proc.stdout + proc.stderr,
        )

    def raw(self, argv):
        proc = subprocess.run([self.args.analyzer] + [str(a) for a in argv],
                              capture_output=True, text=True, timeout=60)
        return SimpleNamespace(rc=proc.returncode, log=proc.stdout + proc.stderr)

    @staticmethod
    def parse(text):
        lines = text.splitlines()
        spans = [ln.split("\t") for ln in lines if ln and not ln.startswith("#")]
        return SimpleNamespace(
            text=text,
            lines=lines,
            spans=spans,
            functions=[row[5] for row in spans if len(row) > 6],
            mangled=[row[6] for row in spans if len(row) > 6],
            entries=[ln[len("#entry "):] for ln in lines
                     if ln.startswith("#entry ")],
            ends=[ln for ln in lines if ln.startswith("#end ")],
        )

    @staticmethod
    def span_count(text, name="main"):
        for line in text.splitlines():
            if line.startswith("#end %s spans=" % name):
                return int(line.split("spans=")[1].split()[0])
        return -1

    def entry_blocks(self, text):
        """Split a multi-entry TSV report into name -> parsed spans."""
        blocks = {}
        name, lines = None, []
        for line in text.splitlines():
            if line.startswith("#entry "):
                name, lines = line[len("#entry "):], []
            elif line.startswith("#end ") and name is not None:
                blocks[name] = self.parse("\n".join(lines))
                name = None
            elif name is not None:
                lines.append(line)
        return blocks

    def filter_file(self, name, content):
        path = self.work / name
        path.write_text(content, encoding="utf-8")
        return str(path)

    def run(self):
        names = sorted(n for n in dir(self) if n.startswith("test_"))
        failed_cases = 0
        for name in names:
            self.current = name
            before = len(self.failures)
            try:
                getattr(self, name)()
            except Exception as exc:  # a broken fixture must fail the case
                self.failures.append("%s: raised %s: %s" %
                                     (name, type(exc).__name__, exc))
            if len(self.failures) > before:
                failed_cases += 1
            print("[%s] %s" % ("FAIL" if len(self.failures) > before else "PASS",
                               name), flush=True)
        for failure in self.failures:
            print("  - " + failure, file=sys.stderr)
        print("\n%d/%d cases passed" % (len(names) - failed_cases, len(names)))
        return 1 if self.failures else 0

    # ---------------------------------------------------------------- cases

    def test_baseline_tsv(self):
        r = self.run_analyzer([self.bc["prog"]])
        p = self.parse(r.text)
        self.expect(r.rc == 0, "analyzer failed")
        self.expect("#schema_version 2" in p.lines, "missing schema header")
        self.expect(any(ln.startswith("#analysis ") and "exclude_std=1" in ln
                        for ln in p.lines), "missing analysis header")
        self.expect("#entries main" in p.lines, "default entry is not main")
        self.expect(p.entries == ["main"], "expected one entry block")
        self.expect(len(p.spans) == 4, "expected 4 spans, got %d" % len(p.spans))
        self.expect(set(p.functions) == {"main", "foo", "bar", "zoo"},
                    "wrong function set: %s" % p.functions)
        self.expect(all(len(row) == 12 for row in p.spans),
                    "TSV rows must have 12 columns")
        self.expect(all(row[4] == "0" for row in p.spans),
                    "unexpected cycle marker")
        self.expect(all(row[7] and row[8] and row[9] for row in p.spans),
                    "debug file/line columns missing with -g")
        self.expect(all(row[10] and row[11] for row in p.spans[1:]),
                    "call-site columns missing with -g")
        self.expect(self.span_count(r.text) == 4 and
                    "reachable_funcs=4" in r.text, "wrong #end summary")

    def test_tree_layout(self):
        r = self.run_analyzer([self.bc["prog"]], fmt="tree")
        self.expect(r.rc == 0, "analyzer failed")
        self.expect("===== entry: main =====" in r.text, "missing tree header")
        self.expect("├── foo" in r.text and "└── zoo" in r.text,
                    "missing branch connectors")
        self.expect("main [root]" in r.text and "[direct]" in r.text,
                    "tree must label the call mode of every span")
        self.expect("test_prog.c:6" in r.text, "missing call-site location")
        self.expect("===== end: main spans=4 reachable=4 =====" in r.text,
                    "wrong tree summary")

    def test_complex_tsv_and_library_exclusion(self):
        r = self.run_analyzer([self.bc["complex"]])
        p = self.parse(r.text)
        self.expect(r.rc == 0, "analyzer failed")
        self.expect(len(p.spans) > 100, "complex fixture lost its call tree")
        self.expect(any(row[2] == "indirect" for row in p.spans),
                    "no indirect call edges")
        self.expect(any(row[4] == "1" for row in p.spans),
                    "no recursion back-edges (cycle column)")
        self.expect(all("std::" not in f for f in p.functions),
                    "std library leaked into the default report")
        self.expect("printf" not in p.functions,
                    "C library leaked into the default report")
        self.expect(self.span_count(r.text) == len(p.spans),
                    "#end span count disagrees with the report")

        inc = self.run_analyzer(["--include-stdlib", self.bc["complex"]])
        pi = self.parse(inc.text)
        self.expect(inc.rc == 0, "include-stdlib run failed")
        self.expect("exclude_std=0" in inc.text, "switch not reflected in header")
        self.expect(len(pi.spans) > len(p.spans),
                    "--include-stdlib should reveal additional spans")
        self.expect(any("std::" in f for f in pi.functions),
                    "--include-stdlib should reveal std:: functions")

    def test_tree_complex_markers(self):
        r = self.run_analyzer([self.bc["complex"]], fmt="tree")
        self.expect("├──" in r.text and "│" in r.text, "missing tree branches")
        self.expect("[indirect]" in r.text, "missing [indirect] marker")
        self.expect("[cycle]" in r.text, "missing [cycle] marker")

    def test_filter_file_and_callback_grafting(self):
        f = self.filter_file("filters_cb.txt",
                             "# comment\n\n  ^testCallbackParam$  \r\n")
        r = self.run_analyzer(["--filter-file", f, self.bc["complex"]])
        p = self.parse(r.text)
        self.expect(r.rc == 0, "analyzer failed")
        self.expect("patterns=1" in r.text,
                    "comment/blank/CRLF lines were not skipped or trimmed")
        self.expect("testCallbackParam" not in p.functions,
                    "filtered function is still reported")
        self.expect("sortWithCallback" in p.functions,
                    "user callee of a hidden function must be grafted")
        self.expect("compareAsc" in p.functions,
                    "callback reachable only through a hidden node disappeared")

    def test_filter_matches_all_name_forms(self):
        # demangled with parameters, raw mangled symbol, parameter-stripped name
        f = self.filter_file("filters_forms.txt",
                             "overloaded\\(int\\)\n"
                             "^_Z10directCallv$\n"
                             "^sortWithCallback$\n")
        r = self.run_analyzer(["--filter-file", f, self.bc["complex"]])
        p = self.parse(r.text)
        self.expect("patterns=3" in r.text, "wrong pattern count")
        self.expect("_Z10overloadedi" not in p.mangled,
                    "demangled-signature regex did not match")
        self.expect("_Z10overloadedd" in p.mangled,
                    "the other overload must stay visible")
        self.expect("_Z10directCallv" not in p.mangled,
                    "mangled-symbol regex did not match")
        self.expect("sortWithCallback" not in p.functions,
                    "parameter-stripped regex did not match")

    def test_invalid_and_missing_filter(self):
        f = self.filter_file("filters_invalid.txt", "[\n^foo$\n")
        r = self.run_analyzer(["--filter-file", f, self.bc["prog"]])
        p = self.parse(r.text)
        self.expect(r.rc == 0, "an invalid pattern must not abort the analysis")
        self.expect("[WARN] ignoring invalid filter pattern" in r.log,
                    "missing warning for invalid regex")
        self.expect("patterns=2" in r.text, "header must count all configured lines")
        self.expect("foo" not in p.functions and "bar" in p.functions,
                    "the valid pattern was not applied")

        missing = str(self.work / "no_such_filter.txt")
        r2 = self.run_analyzer(["--filter-file", missing, self.bc["prog"]])
        self.expect("[ERROR] cannot open filter file" in r2.log,
                    "missing filter file not reported")
        self.expect("#filter_file" not in r2.text,
                    "a missing filter file must not be advertised in the header")
        self.expect(self.span_count(r2.text) == 4,
                    "analysis should continue unfiltered")

    def test_entries_and_entry_file(self):
        r = self.run_analyzer(["--entry", "foo", "--entry", "nope",
                               "--entry", "zoo", self.bc["prog"]])
        p = self.parse(r.text)
        self.expect("#entries foo nope zoo" in r.text, "wrong entry list")
        self.expect(p.entries == ["foo", "zoo"], "one block per resolved entry")
        self.expect(len(p.ends) == 2, "one summary per resolved entry")
        self.expect("[WARN] entry function not found: nope" in r.log,
                    "missing warning for unresolved entry")

        f = self.filter_file("entries.txt", "foo\r\n\nzoo\r\n")
        r2 = self.run_analyzer(["--entry-file", f, self.bc["prog"]])
        self.expect("#entries foo zoo" in r2.text,
                    "CRLF/blank-line entry file not parsed")
        self.expect(self.parse(r2.text).entries == ["foo", "zoo"],
                    "entry file produced wrong blocks")

    def test_mangled_entry(self):
        r = self.run_analyzer(["--entry", "_Z10directCallv", self.bc["complex"]])
        p = self.parse(r.text)
        self.expect(p.entries == ["_Z10directCallv"], "mangled entry not resolved")
        self.expect("directCall" in p.functions, "wrong span for mangled entry")
        self.expect(self.span_count(r.text, "_Z10directCallv") == 1,
                    "unexpected span count for mangled entry")

    def test_entry_hidden_by_filter(self):
        f = self.filter_file("hide_main.txt", "^main$\n")
        r = self.run_analyzer(["--filter-file", f, self.bc["prog"]])
        self.expect("entry function matches a filter pattern, skipped: main" in r.log,
                    "missing warning for filtered entry")
        self.expect(self.parse(r.text).entries == [],
                    "filtered entry must not emit a block")

    def test_output_escaping(self):
        weird = self.work / "we\tird.txt"
        weird.write_text("^zzz$\n", encoding="utf-8")
        r = self.run_analyzer(["--filter-file", weird,
                               "--entry", "a\\b", "--entry", "x\ty",
                               self.bc["prog"]])
        self.expect(r"#entries a\\b x\ty" in r.text,
                    "entry names with backslash/tab were not escaped")
        self.expect("#filter_file " in r.text and r"we\tird.txt" in r.text,
                    "filter-file path with a tab was not escaped")
        self.expect("patterns=1" in r.text, "filter file was not applied")
        self.expect("entry function not found: a\\b" in r.log and
                    "entry function not found: x\ty" in r.log,
                    "missing warnings for unresolved escaped entries")

    def test_no_debug_info(self):
        r = self.run_analyzer([self.bc["prog_nodbg"]])
        p = self.parse(r.text)
        self.expect(r.rc == 0 and len(p.spans) == 4, "release run failed")
        self.expect(set(p.functions) == {"main", "foo", "bar", "zoo"},
                    "a build without -g must not hide user functions")
        self.expect(all(row[7] == "" and row[8] == "" and row[9] == ""
                        for row in p.spans), "line columns must be empty without -g")
        self.expect(all(row[10] == "" and row[11] == "" for row in p.spans),
                    "call-site columns must be empty without -g")
        self.expect(self.span_count(r.text) == 4, "wrong #end summary")

    def test_no_debug_info_library_symbols(self):
        r = self.run_analyzer([self.bc["complex_nodbg"]])
        p = self.parse(r.text)
        self.expect(len(p.spans) > 100, "release C++ fixture lost its call tree")
        self.expect("directCall" in p.functions and "testFuncPtr" in p.functions,
                    "user functions missing without -g")
        self.expect(all("std::" not in f for f in p.functions),
                    "std symbols must still be excluded without debug info")
        self.expect(all(row[8] == "" and row[9] == "" for row in p.spans),
                    "line columns must be empty without -g")

    def test_new_call_scenarios(self):
        # User-defined operators and a functor wrapping a function pointer must
        # produce call edges (the old fixture only had a built-in int +).
        r = self.run_analyzer([self.bc["complex"]])
        p = self.parse(r.text)
        for name in ("Addable::operator+", "Addable::operator*",
                     "opFuncA", "opFuncB"):
            self.expect(name in p.functions, "missing call edge: %s" % name)
        self.expect("_ZNK11CallableOpsclEv" in p.mangled,
                    "missing CallableOps::operator() call edge")

    def test_anonymous_namespace_names(self):
        """(anonymous namespace):: must not be mistaken for a parameter list."""
        src = self.work / "anon_ns.cpp"
        src.write_text(
            "namespace {\n"
            "int helper(int x) { return x + 1; }\n"
            "int top(int x) { return helper(x) * 2; }\n"
            "}\n"
            "int main() { return top(1); }\n",
            encoding="utf-8")
        bc = self.work / "anon_ns.bc"
        subprocess.run([self.args.clangxx, "-std=c++17", "-O0", "-g",
                        "-emit-llvm", "-c", str(src), "-o", str(bc)],
                       check=True, capture_output=True, text=True, timeout=180)
        r = self.run_analyzer([bc])
        p = self.parse(r.text)
        self.expect(r.rc == 0 and len(p.spans) >= 3,
                    "anonymous-namespace fixture analysis failed")
        self.expect(all(f.strip() for f in p.functions),
                    "empty function name in a call-graph span")
        self.expect("(anonymous namespace)::top" in p.functions and
                    "(anonymous namespace)::helper" in p.functions,
                    "anonymous-namespace function lost its name")

    def test_cli_error_paths(self):
        bad = self.raw(["--format", "bogus", self.bc["prog"]])
        self.expect(bad.rc == 1 and "unknown --format" in bad.log,
                    "unknown report format must fail")
        help_ = self.raw(["--help"])
        self.expect(help_.rc == 0 and "Usage:" in help_.log,
                    "--help should print usage and succeed")
        missing = self.raw([self.work / "no_such.bc"])
        self.expect(missing.rc != 0 and "Failed to load bitcode" in missing.log,
                    "missing bitcode must be reported as an error")

    def test_web_service_http_entry_chains(self):
        """Each HTTP entry keeps its own business chain, sharing middleware."""
        get = self.parse(self.run_analyzer(
            ["--entry", "http_get_user", self.bc["web"]]).text)
        post = self.parse(self.run_analyzer(
            ["--entry", "http_post_order", self.bc["web"]]).text)
        shared = ("logRequest", "authenticate", "makeError", "jsonOk")
        get_only = ("parsePathId", "loadUser", "readUserCache",
                    "queryUserTable", "renderUserJson")
        post_only = ("validateOrderJson", "decodeOrder", "reserveInventory",
                     "publishOrderCreated", "renderOrderJson")

        self.expect("http_get_user" in get.functions, "GET entry missing")
        self.expect("http_post_order" in post.functions, "POST entry missing")
        for name in shared + get_only:
            self.expect(any(name in f for f in get.functions),
                        "GET chain missing %s" % name)
        for name in shared + post_only:
            self.expect(any(name in f for f in post.functions),
                        "POST chain missing %s" % name)
        for name in post_only + ("http_post_order",):
            self.expect(not any(name in f for f in get.functions),
                        "GET chain leaked POST-only function %s" % name)
        for name in get_only + ("http_get_user",):
            self.expect(not any(name in f for f in post.functions),
                        "POST chain leaked GET-only function %s" % name)

    def test_web_service_two_entries_report(self):
        r = self.run_analyzer(["--entry", "http_get_user",
                               "--entry", "http_post_order", self.bc["web"]])
        blocks = self.entry_blocks(r.text)
        self.expect(r.rc == 0, "two-entry analysis failed")
        self.expect("#entries http_get_user http_post_order" in r.text,
                    "wrong entry list")
        self.expect(set(blocks) == {"http_get_user", "http_post_order"},
                    "expected one report block per HTTP entry")
        if len(blocks) == 2:
            get, post = blocks["http_get_user"], blocks["http_post_order"]
            self.expect(len(get.spans) > 5 and len(post.spans) > 5,
                        "an HTTP entry has no call chain")
            self.expect(any("loadUser" in f for f in get.functions) and
                        not any("reserveInventory" in f for f in get.functions),
                        "GET block contains the wrong chain")
            self.expect(any("reserveInventory" in f for f in post.functions) and
                        not any("loadUser" in f for f in post.functions),
                        "POST block contains the wrong chain")
            self.expect(
                self.span_count(r.text, "http_get_user") == len(get.spans) and
                self.span_count(r.text, "http_post_order") == len(post.spans),
                "#end span counts disagree with the per-entry blocks")

    def test_web_service_router_indirect_dispatch(self):
        r = self.run_analyzer([self.bc["web"]])  # default entry: main
        p = self.parse(r.text)
        indirect = {row[5] for row in p.spans if row[2] == "indirect"}
        self.expect("dispatch" in p.functions, "router missing from main chain")
        self.expect("http_get_user" in indirect and
                    "http_post_order" in indirect,
                    "router must reach both HTTP entries via indirect calls")

    def test_web_service_live_http(self):
        """The fixture is a real server: exercise both endpoints over TCP."""
        exe = self.work / "web_service"
        subprocess.run(
            [self.args.clangxx, "-std=c++17", "-O0",
             str(Path(self.args.source_dir) / "test_web_service.cpp"),
             "-o", str(exe)],
            check=True, capture_output=True, text=True, timeout=180)
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        proc = subprocess.Popen([str(exe), str(port), "0"],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                if proc.poll() is not None:
                    self.expect(False, "web service exited before listening")
                    return
                try:
                    ready = socket.create_connection(("127.0.0.1", port), 0.2)
                    ready.close()
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                self.expect(False, "web service did not start")
                return

            def request(method, path, token=None, body=None):
                lines = ["%s %s HTTP/1.1" % (method, path),
                         "Host: 127.0.0.1", "Connection: close"]
                if token:
                    lines.append("Authorization: Bearer %s" % token)
                if body is not None:
                    lines.append("Content-Type: application/json")
                    lines.append("Content-Length: %d" % len(body))
                raw = "\r\n".join(lines) + "\r\n\r\n" + (body or "")
                with socket.create_connection(("127.0.0.1", port), 2) as sock:
                    sock.settimeout(3)
                    sock.sendall(raw.encode())
                    chunks = []
                    while True:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        chunks.append(chunk)
                return b"".join(chunks).decode("utf-8", "replace")

            ok = request("GET", "/users/7", "token-ada")
            self.expect("200 OK" in ok and '"Ada Lovelace"' in ok,
                        "GET /users/7 did not return the real user record")
            forbidden = request("GET", "/users/8", "token-ada")
            self.expect("403 Forbidden" in forbidden,
                        "token must not read another user's record")
            unauth = request("GET", "/users/7")
            self.expect("401 Unauthorized" in unauth,
                        "missing bearer token must be rejected")
            created = request("POST", "/orders", "token-ada",
                              '{"userId":7,"itemId":101,"quantity":2}')
            self.expect("201 Created" in created and '"orderId":1001' in created,
                        "POST /orders did not create a real order")
            conflict = request("POST", "/orders", "token-ada",
                               '{"userId":7,"itemId":101,"quantity":99}')
            self.expect("409 Conflict" in conflict,
                        "inventory must reject an over-sized order")
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--analyzer", default=str(REPO / "build" / "clang_analyzer"))
    ap.add_argument("--source-dir", default=str(Path(__file__).resolve().parent))
    ap.add_argument("--clang", default=tool_path("clang"))
    ap.add_argument("--clangxx", default=tool_path("clang++"))
    args = ap.parse_args()
    for tool in (args.analyzer, args.clang, args.clangxx):
        if not Path(tool).is_file():
            print("missing required tool: %s" % tool, file=sys.stderr)
            return 2
    with tempfile.TemporaryDirectory(prefix="analyzer-regression-") as tmp:
        return Suite(args, Path(tmp)).run()


if __name__ == "__main__":
    sys.exit(main())
