#!/usr/bin/env python3
"""End-to-end regression tests for the class_diff CLI."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--clangxx", required=True)
    return ap.parse_args()


classFixture = """
namespace app {{
struct C {{
    int f() const {{ return {value}; }}
}};
}}
int main() {{ app::C c; return c.f(); }}
"""

operatorFixture = """
namespace app {{
struct C {{
    bool operator<(const C&) const {{ return {value}; }}
}};
}}
int main() {{ app::C a, b; return a < b; }}
"""

stringFixture = """
namespace app {{
struct C {{
    const char* s() const {{ return "{value}"; }}
}};
}}
int main() {{ app::C c; return c.s()[0]; }}
"""

anonymousFixture = """
namespace app {{
{padding}struct {{
    int f() {{ return 1; }}
}} anon;
}}
int main() {{ return app::anon.f(); }}
"""

llFixture = """
define i32 @_ZN3app1C1fEii(ptr %this, i32 %a, i32 %b) {{
entry:
  %0 = {op} i32 %a, %b
  ret i32 %0
}}
"""


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.strip() + "\n", encoding="utf-8")


def compile_bc(clangxx, src, out):
    subprocess.run(
        [clangxx, "-std=c++17", "-O0", "-emit-llvm", "-c", str(src), "-o", str(out)],
        check=True,
        capture_output=True,
        text=True,
    )


def make_sources(root, name, old, new):
    old_dir = root / name / "old"
    new_dir = root / name / "new"
    write(old_dir / "a.cpp", old)
    write(new_dir / "a.cpp", new)
    return old_dir, new_dir


def make_bitcode(root, name, old, new, clangxx, debug=False):
    old_dir, new_dir = make_sources(root, name, old, new)
    for directory in (old_dir, new_dir):
        args = [clangxx, "-std=c++17", "-O0", "-emit-llvm", "-c"]
        if debug:
            args.append("-g")
        subprocess.run(
            args + [str(directory / "a.cpp"), "-o", str(directory / "a.bc")],
            check=True,
            capture_output=True,
            text=True,
        )
    return old_dir, new_dir


def run(binary, args, expected=None):
    cp = subprocess.run(
        [binary] + [str(a) for a in args],
        capture_output=True,
        text=True,
    )
    if expected is not None and cp.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {cp.returncode}\n"
            f"args: {args}\nstdout:\n{cp.stdout}\nstderr:\n{cp.stderr}"
        )
    return cp


def json_summary(binary, args, root, new, expected=0):
    cp = run(binary, ["--format", "json"] + args + [root, new], expected)
    return json.loads(cp.stdout)["summary"]


def check_cli(binary):
    cp = run(binary, ["--help"], 0)
    assert "Usage:" in cp.stdout, cp.stdout
    assert cp.stderr == "", cp.stderr
    run(binary, ["--unknown-option", "a", "b"], 1)


def check_source(binary, tmp):
    old, new = make_sources(tmp, "source", classFixture.format(value=1),
                            classFixture.format(value=2))
    summary = json_summary(binary, [], old, new)
    assert summary["changed_classes"] == 1, summary
    assert summary["changed_methods"] == 1, summary

    old, new = make_sources(tmp, "source-same", classFixture.format(value=1),
                            classFixture.format(value=1))
    summary = json_summary(binary, [], old, new)
    assert summary["changed_classes"] == 0, summary

    old, new = make_sources(
        tmp, "line-move",
        anonymousFixture.format(padding="\n\n\n"),
        anonymousFixture.format(padding=""),
    )
    summary = json_summary(binary, [], old, new)
    assert summary["old_classes"] > 0, summary
    assert summary["changed_classes"] == 0, summary

    old, new = make_sources(tmp, "field-change",
                            "namespace app { struct C { int x; }; }\n",
                            "namespace app { struct C { long x; }; }\n")
    cp = run(binary, ["--format", "json", old, new], 0)
    report = json.loads(cp.stdout)
    details = report["classes"][0]["class_details"]
    assert any("fields:" in detail for detail in details), details


def check_bitcode(binary, clangxx, tmp):
    old, new = make_bitcode(tmp, "bc-same", classFixture.format(value=1),
                            classFixture.format(value=1), clangxx)
    summary = json_summary(binary, ["--input-type", "bitcode"], old, new)
    assert summary["changed_classes"] == 0, summary

    for name, fixture, old_value, new_value in (
        ("bc-change", classFixture, 1, 2),
        ("bc-operator", operatorFixture, "false", "true"),
        ("bc-string", stringFixture, "old-constant", "new-constant"),
    ):
        old, new = make_bitcode(tmp, name, fixture.format(value=old_value),
                                fixture.format(value=new_value), clangxx)
        summary = json_summary(binary, ["--input-type", "bitcode"], old, new)
        assert summary["changed_classes"] == 1, (name, summary)
        assert summary["changed_methods"] == 1, (name, summary)

    old_dir = tmp / "bc-nsw" / "old"
    new_dir = tmp / "bc-nsw" / "new"
    write(old_dir / "a.ll", llFixture.format(op="add"))
    write(new_dir / "a.ll", llFixture.format(op="add nsw"))
    summary = json_summary(binary, ["--input-type", "bitcode"], old_dir, new_dir)
    assert summary["changed_methods"] == 1, summary


def check_ignore(binary, clangxx, tmp):
    keep = classFixture.format(value=1)
    old_dir = tmp / "bc-ignore" / "old"
    new_dir = tmp / "bc-ignore" / "new"
    for directory in (old_dir, new_dir):
        source = directory / "skip" / "ignored.cpp"
        write(source, stringFixture.format(value="ignored"))
        subprocess.run(
            [clangxx, "-std=c++17", "-O0", "-g", "-emit-llvm", "-c",
             str(source), "-o", str(directory / "ignored.bc")],
            check=True, capture_output=True, text=True)
        keep_source = directory / "keep.cpp"
        write(keep_source, keep)
        subprocess.run(
            [clangxx, "-std=c++17", "-O0", "-g", "-emit-llvm", "-c",
             str(keep_source), "-o", str(directory / "keep.bc")],
            check=True, capture_output=True, text=True)

    summary = json_summary(
        binary, ["--input-type", "bitcode", "--ignore", "skip"],
        old_dir, new_dir)
    assert summary["changed_classes"] == 0, summary
    assert summary["old_classes"] == 1, summary
    assert summary["new_classes"] == 1, summary


def check_error_paths(binary, clangxx, tmp):
    old, new = make_sources(tmp, "parse-error", "not valid C++\n",
                            "also not valid C++\n")
    cp = run(binary, [old, new], 1)
    assert "failed to parse" in cp.stderr, cp.stderr

    old, new = make_bitcode(tmp, "debug-mismatch", classFixture.format(value=1),
                            classFixture.format(value=1), clangxx, debug=False)
    cp = subprocess.run(
        [clangxx, "-std=c++17", "-O0", "-g", "-emit-llvm", "-c",
         str(new / "a.cpp"), "-o", str(new / "a.bc")],
        capture_output=True, text=True)
    assert cp.returncode == 0, cp.stderr
    cp = run(binary, ["--input-type", "bitcode", old, new], 1)
    assert "DWARF debug info differs" in cp.stderr, cp.stderr

    old, new = make_sources(tmp, "output", classFixture.format(value=1),
                            classFixture.format(value=1))
    full = Path("/dev/full")
    if full.exists():
        cp = run(binary, ["--output", str(full), old, new], 1)
        assert "failed to write report" in cp.stderr, cp.stderr


def main():
    args = parse_args()
    with tempfile.TemporaryDirectory(prefix="class_diff_test_") as tmpdir:
        tmp = Path(tmpdir)
        check_cli(args.binary)
        check_source(args.binary, tmp)
        check_bitcode(args.binary, args.clangxx, tmp)
        check_ignore(args.binary, args.clangxx, tmp)
        check_error_paths(args.binary, args.clangxx, tmp)
    print("class_diff regression tests passed")


if __name__ == "__main__":
    sys.exit(main())
