#!/usr/bin/env python3
"""End-to-end tests for entry_extractor.

The fixtures are self-contained: LLVM IR snippets are assembled with llvm-as,
and the C++ fixtures (entry_extractor_fixture.cpp + _aux.cpp) are compiled with
the LLVM-21 clang, linked with llvm-link for the cross-TU case, and also built
and run natively. The C++ source carries EXPECT_ENTRY / EXPECT_ABSENT
declarations, which this driver parses and checks against the extractor's
verbose output. Together they cover the entry patterns a network service uses
to register its request handlers:

  * IR fixtures: string labels, integral (opcode/enum) keys, merged string
    pools addressed with a GEP, handler pointers in another global, vtables /
    RTTI as a negative case, llvm.global_ctors, llvm.global.annotations,
    llvm.used and GlobalAlias;
  * C++ fixture: compile-time tables, std::map / unordered_map / vector
    registries, std::function wrappers, middleware chains, static
    self-registration, constructor attributes, annotations, used symbols,
    pthread_create / signal / atexit callbacks, a pure address-taken callback
    and a vtable-based handler interface (negative case);
  * cross-TU table whose label and handler pointers live in different globals;
  * a handler that is also called directly (previously invisible in every mode);
  * release bitcode without -g keeps its callbacks;
  * the C++ fixture is also built and run natively, proving it is live code;
  * --verbose triggers, plain --entry-file-compatible output, CLI errors;
  * the repository's synchronous and asynchronous HTTP service fixtures;
  * extended labels (negative / 64-bit keys, UTF-8, wide strings, --all-labels);
  * local value-flow registration (select, load/store, returned pointers,
    memcpy), custom sections, opt-in vtable handlers and multi-module input;
  * type-erased lambda handlers, reachability-based roots, exported visibility,
    --filter-file / --entry-name, JSON reports and -O2 stability warnings.

Run through CTest (see CMakeLists.txt) or directly:
    python3 tests/entry_extractor_test.py --extractor <build>/entry_extractor
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_LLVM_BIN = Path("/usr/lib/llvm-21/bin")


def tool_path(name):
    candidate = DEFAULT_LLVM_BIN / name
    if candidate.is_file():
        return str(candidate)
    return shutil.which(name) or shutil.which(name + "-21") or str(candidate)


# ---------------------------------------------------------------------------
# Hand-written IR fixtures. Opaque-pointer syntax (LLVM 21).
# ---------------------------------------------------------------------------

ROUTE_TABLES = r"""
@.str.users = private constant [7 x i8] c"/users\00", align 1
@.str.orders = private constant [8 x i8] c"/orders\00", align 1

; (1) classic string-labelled table
@route_table = internal constant [2 x { ptr, ptr }]
  [{ ptr, ptr } { ptr @.str.users, ptr @h_get_user },
   { ptr, ptr } { ptr @.str.orders, ptr @h_post_order }], align 8

; (2) integral dispatch keys (opcode / message id / enum method)
@op_table = internal constant [2 x { i32, ptr }]
  [{ i32, ptr } { i32 1, ptr @h_ping },
   { i32, ptr } { i32 2, ptr @h_data }], align 8

; (3) merged string pool: two labels share one constant, selected by GEP offset
@.pool = private constant [15 x i8] c"/users\00/orders\00", align 1
@pool_table = internal constant [2 x { ptr, ptr }]
  [{ ptr, ptr } { ptr getelementptr inbounds (i8, ptr @.pool, i64 0), ptr @h_pool_get },
   { ptr, ptr } { ptr getelementptr inbounds (i8, ptr @.pool, i64 7), ptr @h_pool_post }], align 8

; (4) handler pointer lives in a separate global constant
@.str.split = private constant [3 x i8] c"/x\00", align 1
@handler_slot = internal constant ptr @h_split, align 8
@split_table = internal constant { ptr, ptr }
  { ptr @.str.split, ptr @handler_slot }, align 8

; (5) a vtable with an RTTI type-name string must NOT be a route table
@.type_name = private constant [7 x i8] c"5VHttp\00", align 1
@rtti = internal constant { ptr, ptr } { ptr null, ptr @.type_name }, align 8
@vtable = internal constant { ptr, ptr, ptr }
  { ptr null, ptr @rtti, ptr @h_virtual }, align 8

define void @h_get_user(i32 %x) { ret void }
define void @h_post_order(i32 %x) { ret void }
define void @h_ping(i32 %x) { ret void }
define void @h_data(i32 %x) { ret void }
define void @h_pool_get(i32 %x) { ret void }
define void @h_pool_post(i32 %x) { ret void }
define void @h_split(i32 %x) { ret void }
define void @h_virtual(i32 %x) { ret void }
define i32 @main() { ret i32 0 }
"""

METADATA = r"""
@.str.a = private constant [3 x i8] c"/a\00", align 1
@.ann = private constant [9 x i8] c"route:/x\00", align 1
@.src = private constant [8 x i8] c"test.ll\00", align 1

@llvm.global.annotations = appending global
  [1 x { ptr, ptr, ptr, i32, ptr }]
  [{ ptr, ptr, ptr, i32, ptr }
   { ptr @h_annotated, ptr @.ann, ptr @.src, i32 7, ptr null }]

@llvm.used = appending global [1 x ptr] [ptr @h_retained]

@h_alias = alias void (i32), ptr @h_aliased
@alias_table = internal constant { ptr, ptr }
  { ptr @.str.a, ptr @h_alias }, align 8

define void @h_annotated(i32 %x) { ret void }
define void @h_retained(i32 %x) { ret void }
define void @h_aliased(i32 %x) { ret void }
define i32 @main() { ret i32 0 }
"""

CTORS = r"""
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }]
  [{ i32, ptr, ptr } { i32 65535, ptr @ctor_init, ptr null }]

@.str.health = private constant [8 x i8] c"/health\00", align 1

declare void @register_handler(ptr, ptr)

define void @h_from_ctor(i32 %x) { ret void }
define void @ctor_init() {
  call void @register_handler(ptr @.str.health, ptr @h_from_ctor)
  ret void
}
define i32 @main() { ret i32 0 }
"""

# Extended label decoding: negative / 64-bit opcode keys, UTF-8 and wide
# strings, plus a table carrying both a string and an integer key.
EXTENDED_LABELS = r"""
@.str.ok = private constant [4 x i8] c"/ok\00", align 1
@.str.utf8 = private constant [6 x i8] c"/u\E7\94\A8\00", align 1
@.str.wide = private constant [2 x i16] [i16 47, i16 0], align 2

@ok_table = internal constant { ptr, ptr } { ptr @.str.ok, ptr @h_ok }, align 8
@neg_table = internal constant { i32, ptr } { i32 -1, ptr @h_neg }, align 8
@big_table = internal constant { i64, ptr } { i64 4294967300, ptr @h_big }, align 8
@utf_table = internal constant { ptr, ptr } { ptr @.str.utf8, ptr @h_utf }, align 8
@wide_table = internal constant { ptr, ptr } { ptr @.str.wide, ptr @h_wide }, align 8
@mixed_table = internal constant { ptr, i32, ptr }
  { ptr @.str.ok, i32 7, ptr @h_mixed }, align 8

define void @h_ok(i32 %x) { ret void }
define void @h_neg(i32 %x) { ret void }
define void @h_big(i32 %x) { ret void }
define void @h_utf(i32 %x) { ret void }
define void @h_wide(i32 %x) { ret void }
define void @h_mixed(i32 %x) { ret void }
define i32 @main() { ret i32 0 }
"""

# Registration through a local value flow: select, load/store, a function
# returning a pointer, and a memcpy-style table copy.
VALUE_FLOW = r"""
@slot = internal global ptr null, align 8
@tbl = internal constant [1 x ptr] [ptr @h_memcpy], align 8
@dest = internal global [1 x ptr] zeroinitializer, align 8

declare void @reg(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg)

define void @h_load(i32 %x) { ret void }
define void @h_select(i32 %x) { ret void }
define void @h_direct(i32 %x) { ret void }
define void @h_return(i32 %x) { ret void }
define void @h_memcpy(i32 %x) { ret void }

define ptr @choose() {
  ret ptr @h_return
}

define void @install() {
  %c = icmp eq i32 1, 1
  %sel = select i1 %c, ptr @h_select, ptr @h_direct
  call void @reg(ptr %sel)
  store ptr @h_load, ptr @slot, align 8
  %p = load ptr, ptr @slot, align 8
  call void @reg(ptr %p)
  %r = call ptr @choose()
  call void @reg(ptr %r)
  call void @llvm.memcpy.p0.p0.i64(ptr @dest, ptr @tbl, i64 8, i1 false)
  %q = load ptr, ptr @dest, align 8
  call void @reg(ptr %q)
  ret void
}

define i32 @main() {
  call void @install()
  ret i32 0
}
"""

# A linker-set style table in a custom section, and an Itanium vtable.
SECTION_TABLE = r"""
@p_init = internal constant ptr @section_init, section ".init_array", align 8

define void @section_init() { ret void }
define i32 @main() {
  call void @section_init()
  ret i32 0
}
"""

VIRTUAL_TABLE = r"""
@_ZTV7Handler = internal constant [1 x ptr] [ptr @v_handler], align 8

define void @v_handler() { ret void }
define i32 @main() { ret i32 0 }
"""

MULTI_MODULE_A = r"""
define void @alpha_handler(i32 %x) { ret void }
define i32 @main() { ret i32 0 }
"""

MULTI_MODULE_B = r"""
define void @beta_handler(i32 %x) { ret void }
"""

# The C++ fixtures live in their own source files next to this driver:
#   entry_extractor_fixture.cpp      — a working network-service model carrying
#                                      EXPECT_ENTRY / EXPECT_ABSENT declarations
#   entry_extractor_fixture_aux.cpp — second TU for the cross-global table
# The driver compiles them to bitcode (with and without -g), links both TUs for
# the cross-TU case, and can also build/run them natively.

EXPECT_ENTRY_RE = re.compile(
    r"//\s*EXPECT_ENTRY\s+name=(.+?)\s+reason=(\S+)"
    r"(?:\s+trigger=(\S+))?\s*$")
EXPECT_ABSENT_RE = re.compile(
    r"//\s*EXPECT_ABSENT\s+mode=(\S+)\s+name=(.+?)\s*$")


class Suite:
    def __init__(self, args, work):
        self.args = args
        self.work = work
        self.failures = []
        self.current = ""
        self.skipped = []
        self.bc = {}
        self.cpp = {}

        for name, source in (("routes", ROUTE_TABLES), ("metadata", METADATA),
                             ("ctors", CTORS),
                             ("extended_labels", EXTENDED_LABELS),
                             ("value_flow", VALUE_FLOW),
                             ("section", SECTION_TABLE),
                             ("virtual", VIRTUAL_TABLE),
                             ("multi_a", MULTI_MODULE_A),
                             ("multi_b", MULTI_MODULE_B)):
            self.bc[name] = self.assemble(name, source)

        self.cpp_fixture = Path(args.source_dir) / "entry_extractor_fixture.cpp"
        self.cpp_aux = Path(args.source_dir) / "entry_extractor_fixture_aux.cpp"
        for path in (self.cpp_fixture, self.cpp_aux):
            if not path.is_file():
                raise RuntimeError("C++ fixture missing: %s" % path)
        self.expect_entries = []
        self.expect_absent = []
        self.parse_expectations()

        for key, debug in (("fixture", True), ("fixture_nodbg", False)):
            self.cpp[key] = self.compile_cpp(self.cpp_fixture, key, debug)

        main_tu = self.compile_cpp(self.cpp_fixture, "fixture_main_tu", True)
        aux_tu = self.compile_cpp(self.cpp_aux, "fixture_aux_tu", True)
        self.cpp["cross_tu"] = self.link([main_tu, aux_tu], "fixture_linked")

        advanced = Path(args.source_dir) / "entry_extractor_advanced_fixture.cpp"
        self.expect(advanced.is_file(), "advanced C++ fixture missing")
        self.cpp["advanced"] = self.compile_cpp(advanced, "advanced", True)
        self.cpp["advanced_opt"] = self.compile_cpp(
            advanced, "advanced_opt", True, extra=["-O2"])

    # -- expectation declarations in the C++ fixture -----------------------

    def parse_expectations(self):
        for line in self.cpp_fixture.read_text(encoding="utf-8").splitlines():
            if "<" in line or ">" in line:  # skip the documentation examples
                continue
            match = EXPECT_ENTRY_RE.search(line)
            if match:
                self.expect_entries.append({
                    "name": match.group(1).strip(),
                    "reason": match.group(2).strip(),
                    "trigger": (match.group(3) or "").strip(),
                })
                continue
            match = EXPECT_ABSENT_RE.search(line)
            if match:
                self.expect_absent.append({
                    "mode": match.group(1).strip(),
                    "name": match.group(2).strip(),
                })
        if len(self.expect_entries) < 20:
            raise RuntimeError("expected >= 20 EXPECT_ENTRY declarations in %s, "
                               "found %d" %
                               (self.cpp_fixture, len(self.expect_entries)))

    def check_expectations(self, key, label):
        rows = self.roots(self.cpp[key])
        for spec in self.expect_entries:
            row = self.exact(rows, spec["name"])
            self.expect(row is not None,
                        "%s: missing entry %s" % (label, spec["name"]))
            if not row:
                continue
            self.expect(row[1] == spec["reason"],
                        "%s: %s reason %s, expected %s" %
                        (label, spec["name"], row[1], spec["reason"]))
            if spec["trigger"]:
                trigger = row[2] if len(row) > 2 else ""
                self.expect(spec["trigger"] in trigger,
                            "%s: %s trigger %r lacks %r" %
                            (label, spec["name"], trigger, spec["trigger"]))
        for spec in self.expect_absent:
            if spec["mode"] == "routes":
                rows_in_mode = self.rows(self.cpp[key], "--demangle")
            else:
                rows_in_mode = rows
            self.expect(self.find(rows_in_mode, spec["name"]) is None,
                        "%s: %s unexpectedly appears in %s mode" %
                        (label, spec["name"], spec["mode"]))

    # -- fixture builders --------------------------------------------------

    def assemble(self, name, text):
        ll = self.work / (name + ".ll")
        bc = self.work / (name + ".bc")
        ll.write_text(text, encoding="utf-8")
        subprocess.run([self.args.llvm_as, str(ll), "-o", str(bc)],
                       check=True, capture_output=True, text=True, timeout=60)
        return bc

    def compile_cpp(self, source, stem, debug, extra=None):
        out = self.work / (stem + ".bc")
        cmd = [self.args.clangxx, "-std=c++17", "-O0", "-emit-llvm", "-c"]
        if debug:
            cmd.append("-g")
        if extra:
            cmd += extra
        cmd += [str(source), "-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
        if proc.returncode != 0:
            raise RuntimeError("compile failed for %s: %s" %
                               (source.name, proc.stderr[-800:]))
        return out

    def link(self, inputs, stem):
        out = self.work / (stem + ".bc")
        subprocess.run([self.args.llvm_link] +
                       [str(path) for path in inputs] + ["-o", str(out)],
                       check=True, capture_output=True, text=True, timeout=60)
        return out

    def compile_native(self, stem, extra=None):
        out = self.work / stem
        cmd = [self.args.clangxx, "-std=c++17", "-O0", "-pthread"] + \
              (extra or []) + \
              [str(self.cpp_fixture), str(self.cpp_aux), "-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
        if proc.returncode != 0:
            raise RuntimeError("native build failed: %s" % proc.stderr[-800:])
        return out

    # -- extractor helpers -------------------------------------------------

    def expect(self, condition, message):
        if not condition:
            self.failures.append("%s: %s" % (self.current, message))

    def run(self, argv, check=True):
        proc = subprocess.run([self.args.extractor] + [str(a) for a in argv],
                              capture_output=True, text=True, timeout=180)
        if check:
            self.expect(proc.returncode == 0,
                        "extractor failed: %s" % (proc.stderr[-300:]))
        return proc

    def rows(self, bc, *flags):
        proc = self.run(["--mode", "routes", "--verbose"] + list(flags) +
                        [str(bc)])
        return [line.split("\t") for line in proc.stdout.splitlines()]

    def roots(self, bc, *flags):
        proc = self.run(["--mode", "roots", "--demangle", "--verbose"] +
                        list(flags) + [str(bc)])
        return [line.split("\t") for line in proc.stdout.splitlines()]

    @staticmethod
    def find(rows, needle):
        return next((r for r in rows if needle in r[0]), None)

    @staticmethod
    def exact(rows, name):
        # "ctor_registered" is a substring of "constructor_registered", so
        # EXPECT_ENTRY names need a whole-name match (without the param list).
        for row in rows:
            if row[0] == name or row[0].split("(")[0].strip() == name:
                return row
        return None

    def trigger(self, rows, needle):
        row = self.find(rows, needle)
        return row[2] if row and len(row) > 2 else ""

    # -- tests -------------------------------------------------------------

    def test_constant_route_tables(self):
        rows = self.rows(self.bc["routes"])
        expected = {
            "h_get_user": ("route-handler", "/users"),
            "h_post_order": ("route-handler", "/orders"),
            "h_ping": ("route-handler", "#1"),
            "h_data": ("route-handler", "#2"),
            "h_pool_get": ("route-handler", "/users"),
            "h_pool_post": ("route-handler", "/orders"),
            "h_split": ("route-handler", "/x"),
        }
        for name, (reason, label) in expected.items():
            row = self.find(rows, name)
            self.expect(row is not None, "missing entry %s" % name)
            if row:
                self.expect(row[1] == reason,
                            "%s should be %s, got %s" % (name, reason, row[1]))
                self.expect(label in self.trigger(rows, name),
                            "%s trigger should contain %r, got %r" %
                            (name, label, self.trigger(rows, name)))
        self.expect(self.find(rows, "h_virtual") is None,
                    "a vtable/RTTI global must not be a route table")

    def test_metadata_globals(self):
        rows = self.rows(self.bc["metadata"])
        ann = self.find(rows, "h_annotated")
        self.expect(ann is not None and ann[1] == "annotated",
                    "llvm.global.annotations function missing")
        self.expect("route:/x" in self.trigger(rows, "h_annotated"),
                    "annotation text should be the trigger")
        used = self.find(rows, "h_retained")
        self.expect(used is not None and used[1] == "retained",
                    "llvm.used function missing")
        self.expect("llvm.used" in self.trigger(rows, "h_retained"),
                    "llvm.used should be the trigger")
        aliased = self.find(rows, "h_aliased")
        self.expect(aliased is not None and aliased[1] == "route-handler",
                    "GlobalAlias in a route table not resolved")

    def test_global_ctor_registration(self):
        rows = self.rows(self.bc["ctors"])
        row = self.find(rows, "h_from_ctor")
        self.expect(row is not None and row[1] == "registered-handler",
                    "handler registered from llvm.global_ctors missing")
        trigger = self.trigger(rows, "h_from_ctor")
        self.expect("ctor:" in trigger and "register_handler" in trigger,
                    "ctor registration trigger missing, got %r" % trigger)
        roots = self.roots(self.bc["ctors"])
        from_ctor = self.find(roots, "h_from_ctor")
        self.expect(from_ctor is not None and
                    from_ctor[1] == "registered-handler",
                    "roots mode should also report the ctor-registered handler")
        self.expect(self.find(rows, "ctor_init") is None,
                    "the constructor itself is not a request handler")

    def test_cpp_fixture_expectations(self):
        # Every EXPECT_ENTRY / EXPECT_ABSENT declaration in the C++ file is
        # checked against the roots report of the -g build.
        self.check_expectations("fixture", "C++ fixture (-g)")

    def test_cpp_fixture_without_debug_info(self):
        # The same expectations must hold without DWARF: callbacks no longer
        # depend on -g, and the registered handlers never did.
        self.check_expectations("fixture_nodbg", "C++ fixture (no -g)")

    def test_cpp_fixture_native_execution(self):
        # The fixture is a working program: every table and registry can
        # actually dispatch. A segfault (e.g. static init order) invalidates it.
        exe = self.compile_native("fixture_native")
        proc = subprocess.run([str(exe)], capture_output=True, text=True,
                              timeout=60)
        self.expect(proc.returncode == 0,
                    "C++ fixture must run natively, rc=%d %s" %
                    (proc.returncode, proc.stderr[-300:]))

    def test_cross_tu_dispatch_table(self):
        # Label in one TU's initializer, handler pointers in another global
        # defined by the second TU. llvm-link makes them one module.
        rows = self.rows(self.cpp["cross_tu"], "--demangle")
        for name in ("aux_ping_handler", "aux_pong_handler"):
            row = self.find(rows, name)
            self.expect(row is not None and row[1] == "route-handler",
                        "cross-TU handler %s missing or misclassified" % name)
            self.expect("/rpc" in self.trigger(rows, name),
                        "cross-TU trigger should carry /rpc, got %r" %
                        self.trigger(rows, name))

    def test_runtime_registration(self):
        for key in ("fixture", "fixture_nodbg"):
            rows = self.rows(self.cpp[key], "--demangle")
            for name in ("map_get_user(", "map_post_order(", "opcode_start(",
                         "middleware_auth(", "api_health(", "shared_handler("):
                row = self.find(rows, name)
                self.expect(row is not None and
                            row[1] == "registered-handler",
                            "%s: runtime handler %s missing" % (key, name))
            self.expect("/shared" in self.trigger(rows, "shared_handler("),
                        "%s: registration label /shared missing" % key)
            self.expect("/health" in self.trigger(rows, "api_health("),
                        "%s: registration label /health missing" % key)

    def test_shared_handler_with_direct_caller(self):
        # The old extractor dropped `shared_handler`: it had a direct caller, so
        # it was neither a no-caller root nor a callback, and no route table
        # mentioned it. Runtime registration must recover it in every mode.
        for mode in ("routes", "roots"):
            proc = self.run(["--mode", mode, "--demangle", "--verbose",
                             str(self.cpp["fixture"])])
            self.expect(any("shared_handler(" in line and
                            "registered-handler" in line
                            for line in proc.stdout.splitlines()),
                        "%s: shared handler still missing" % mode)

    def test_callbacks_without_debug_info(self):
        roots = self.roots(self.cpp["fixture_nodbg"])
        loose = self.find(roots, "loose_cb")
        self.expect(loose is not None,
                    "release bitcode lost its address-taken callback")
        if loose:
            self.expect(loose[1] == "callback",
                        "loose_cb reason should be callback, got %s" % loose[1])
        # Debug info must not change the top-level handler set.
        with_g = {r[0] for r in self.roots(self.cpp["fixture"])}
        no_g = {r[0] for r in roots}
        for name in ("map_get_user(", "api_health(", "shared_handler(",
                     "loose_cb", "http_get_user("):
            self.expect(any(name in n for n in with_g) and
                        any(name in n for n in no_g),
                        "%s missing from one of the roots builds" % name)

    def test_output_format_and_cli(self):
        plain = self.run(["--mode", "routes", str(self.bc["routes"])])
        self.expect(all("\t" not in line for line in
                        plain.stdout.splitlines()),
                    "default output must be an --entry-file-compatible name list")
        self.expect(any("h_get_user" in line for line in
                        plain.stdout.splitlines()),
                    "default output lost a handler")

        out = self.work / "entries.txt"
        self.run(["--mode", "routes", "--output", str(out),
                  str(self.bc["routes"])])
        self.expect(out.read_text(encoding="utf-8") == plain.stdout,
                    "--output content differs from stdout")

        demangled = self.run(["--mode", "routes", "--demangle",
                              str(self.cpp["fixture"])])
        self.expect("map_get_user(" in demangled.stdout,
                    "--demangle did not demangle C++ names")

        help_proc = self.run(["--help"], check=False)
        self.expect(help_proc.returncode == 0 and
                    "Usage:" in (help_proc.stdout + help_proc.stderr),
                    "--help should print usage")
        missing = self.run([str(self.work / "does_not_exist.bc")], check=False)
        self.expect(missing.returncode != 0,
                    "missing bitcode must be an error")
        bad_mode = self.run(["--mode", "bogus", str(self.bc["routes"])],
                            check=False)
        self.expect(bad_mode.returncode != 0, "unknown --mode must be an error")

        all_rows = self.run(["--mode", "all", str(self.bc["routes"])]).stdout
        self.expect(all(line in all_rows.splitlines() for line in
                        plain.stdout.splitlines()),
                    "--mode all must be a superset of routes")
        exported = self.run(["--mode", "exported", str(self.bc["routes"])])
        self.expect("main" in exported.stdout.splitlines(),
                    "exported mode should contain main")

    def test_web_service_fixture(self):
        src = Path(self.args.source_dir) / "test_web_service.cpp"
        if not src.is_file():
            self.skipped.append("web service fixture not found")
            return
        bc = self.compile_cpp(src, "web_service", True)
        rows = self.rows(bc, "--demangle")
        get = self.find(rows, "http_get_user")
        post = self.find(rows, "http_post_order")
        self.expect(get is not None and get[1] == "route-handler",
                    "GET endpoint missing from route table")
        self.expect(post is not None and post[1] == "route-handler",
                    "POST endpoint missing from route table")
        self.expect("GET" in self.trigger(rows, "http_get_user") and
                    "/users/" in self.trigger(rows, "http_get_user"),
                    "GET trigger should carry method and path")
        self.expect("POST" in self.trigger(rows, "http_post_order") and
                    "/orders" in self.trigger(rows, "http_post_order"),
                    "POST trigger should carry method and path")

    def test_async_http_service_fixture(self):
        src = Path(self.args.source_dir) / "asio_http_service.cpp"
        if not src.is_file():
            self.skipped.append("asio fixture not found")
            return
        try:
            bc = self.compile_cpp(src, "asio_service", True,
                                  extra=["-pthread"])
        except RuntimeError as error:
            self.skipped.append("asio fixture unavailable: %s" % error)
            return
        rows = self.rows(bc, "--demangle")
        for name in ("handleHealth", "httpGetUser", "listOrders",
                     "httpPostOrder"):
            self.expect(self.find(rows, name) is not None,
                        "async route %s missing" % name)
        self.expect("GET|/health" in self.trigger(rows, "handleHealth"),
                    "async route trigger should be method|path, got %r" %
                    self.trigger(rows, "handleHealth"))
        roots = self.roots(bc)
        self.expect(not any("__cxx_global_var_init" in r[0] for r in roots),
                    "compiler ctor stubs should not be reported as callbacks")
        self.expect(len(roots) < 12,
                    "async roots list is noisy (%d entries)" % len(roots))

    # -- extended coverage --------------------------------------------------

    def test_extended_label_decoding(self):
        rows = self.rows(self.bc["extended_labels"])
        expected = {
            "h_ok": "/ok",
            "h_neg": "#-1",
            "h_big": "#4294967300",
            "h_utf": "/u用",
            "h_wide": "/",
        }
        for name, label in expected.items():
            row = self.find(rows, name)
            self.expect(row is not None and row[1] == "route-handler",
                        "%s should be a route-handler" % name)
            self.expect(label in self.trigger(rows, name),
                        "%s trigger %r lacks %r" %
                        (name, self.trigger(rows, name), label))
        proc = self.run(["--mode", "routes", "--verbose", "--all-labels",
                         str(self.bc["extended_labels"])])
        mixed = [line for line in proc.stdout.splitlines()
                 if "h_mixed" in line]
        self.expect(bool(mixed) and "#7" in mixed[0],
                    "--all-labels should keep integer keys beside strings")

    def test_value_flow_registration(self):
        rows = self.rows(self.bc["value_flow"])
        for name in ("h_select", "h_direct", "h_load", "h_return",
                     "h_memcpy"):
            row = self.find(rows, name)
            self.expect(row is not None and row[1] == "registered-handler",
                        "%s missing from value-flow registration" % name)

    def test_section_and_virtual_handlers(self):
        row = self.find(self.rows(self.bc["section"]), "section_init")
        self.expect(row is not None and row[1] == "section-handler",
                    "custom-section handler missing")
        self.expect(self.find(self.rows(self.bc["virtual"]), "v_handler") is None,
                    "vtable handler must be opt-in")
        enabled = self.run(["--mode", "routes", "--verbose",
                            "--virtual-handlers", str(self.bc["virtual"])])
        self.expect("v_handler" in enabled.stdout and
                    "virtual-handler" in enabled.stdout,
                    "--virtual-handlers should report the vtable function")

    def test_multiple_modules(self):
        both = self.run(["--mode", "roots", str(self.bc["multi_a"]),
                         str(self.bc["multi_b"])])
        names = both.stdout.splitlines()
        self.expect("alpha_handler" in names and "beta_handler" in names,
                    "multiple bitcode modules must be linked in memory")
        single = self.run(["--mode", "roots", str(self.bc["multi_a"])])
        self.expect("beta_handler" not in single.stdout.splitlines(),
                    "a single module must not contain the other TU's handler")

    def test_json_report(self):
        import json
        proc = self.run(["--mode", "routes", "--format", "json",
                         str(self.cpp["fixture"])])
        try:
            report = json.loads(proc.stdout)
        except ValueError as error:
            self.expect(False, "invalid JSON: %s" % error)
            return
        self.expect(report.get("mode") == "routes", "JSON mode field wrong")
        self.expect(isinstance(report.get("warnings"), list),
                    "JSON warnings missing")
        entries = report.get("entries", [])
        self.expect(bool(entries), "JSON has no entries")
        for key in ("name", "mangled", "demangled", "reason", "trigger",
                    "signature", "file", "line", "end_line"):
            self.expect(key in entries[0], "JSON entry lacks %s" % key)
        self.expect(any("http_get_user" in e["mangled"] for e in entries),
                    "JSON lost a route handler")
        self.expect(any(e["file"] and e["line"] > 0 for e in entries),
                    "JSON should carry DWARF source locations")

    def test_filter_and_entry_name(self):
        patterns = self.work / "advanced_filters.txt"
        patterns.write_text("# drop one handler\n^h_load$\n",
                            encoding="utf-8")
        filtered = self.run(["--mode", "routes", "--verbose", "--filter-file",
                             str(patterns), str(self.bc["value_flow"])])
        self.expect("h_load" not in filtered.stdout,
                    "--filter-file did not exclude h_load")
        named = self.run(["--mode", "roots", "--verbose", "--entry-name",
                          "h_sel*", str(self.bc["value_flow"])])
        self.expect(any("h_select" in line and "program-entry" in line
                        for line in named.stdout.splitlines()),
                    "--entry-name should mark h_select as a program entry")

    def test_type_erased_and_reachability(self):
        rows = self.run(["--mode", "routes", "--demangle", "--verbose",
                         str(self.cpp["advanced"])])
        self.expect("$_0" in rows.stdout and
                    "registered-handler" in rows.stdout,
                    "capturing lambda target not recovered")
        disabled = self.run(["--mode", "routes", "--demangle", "--verbose",
                             "--no-type-erased", str(self.cpp["advanced"])])
        self.expect("$_0" not in disabled.stdout,
                    "--no-type-erased should drop the lambda target")
        roots = self.run(["--mode", "roots", str(self.cpp["advanced"])])
        self.expect("dead_handler" in roots.stdout.splitlines(),
                    "dead-code direct callers must not hide an entry")
        legacy = self.run(["--mode", "roots", "--legacy-roots",
                           str(self.cpp["advanced"])])
        self.expect("dead_handler" not in legacy.stdout.splitlines(),
                    "--legacy-roots should restore the old behaviour")
        exported = self.run(["--mode", "exported",
                             str(self.cpp["advanced"])])
        names = exported.stdout.splitlines()
        self.expect("hidden_api" not in names,
                    "hidden-visibility symbol must not be exported")
        self.expect("alias_target" in names,
                    "target of an external alias must be exported")

    def test_optimized_bitcode_diagnostics(self):
        import json
        proc = self.run(["--mode", "roots", "--format", "json",
                         str(self.cpp["advanced_opt"])])
        try:
            report = json.loads(proc.stdout)
        except ValueError as error:
            self.expect(False, "invalid JSON: %s" % error)
            return
        self.expect(any("optimised" in w or "debug info" in w
                        for w in report.get("warnings", [])),
                    "-O2 bitcode should carry a stability warning")
        self.expect(bool(report.get("entries")),
                    "-O2 bitcode produced no entries")

    # -- driver ------------------------------------------------------------

    def run_all(self):
        tests = [name for name in dir(self) if name.startswith("test_")]
        for name in sorted(tests):
            self.current = name
            before = len(self.failures)
            try:
                getattr(self, name)()
            except Exception as error:  # a broken fixture is a test failure
                self.expect(False, "raised %s: %s" %
                            (type(error).__name__, error))
            status = "OK" if len(self.failures) == before else "FAIL"
            print("[%s] %s" % (status, name))
        return self.failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extractor", default=tool_path("entry_extractor"))
    parser.add_argument("--llvm-as", default=tool_path("llvm-as"))
    parser.add_argument("--llvm-link", default=tool_path("llvm-link"))
    parser.add_argument("--clangxx", default=tool_path("clang++"))
    parser.add_argument("--source-dir",
                        default=str(Path(__file__).resolve().parent))
    args = parser.parse_args()

    if not Path(args.extractor).is_file():
        parser.error("entry_extractor not found: %s" % args.extractor)

    with tempfile.TemporaryDirectory(prefix="entry_extractor_test_") as tmp:
        failures = Suite(args, Path(tmp)).run_all()

    if failures:
        print("\n%d failure(s):" % len(failures))
        for failure in failures:
            print("  - " + failure)
        return 1
    print("\nAll entry_extractor tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
