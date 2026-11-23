#!/usr/bin/env python3
"""End-to-end regression tests for the class_extractor CLI."""

import argparse
import json
import os
import subprocess
import sys
import tempfile


def run(extractor, args):
    return subprocess.run(
        [extractor] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--extractor", required=True)
    args = parser.parse_args()
    extractor = args.extractor
    failures = []

    with tempfile.TemporaryDirectory() as td:
        missing = os.path.join(td, "missing.cpp")
        result = run(extractor, ["--format", "text", missing])
        if result.returncode == 0:
            failures.append("a missing input file must fail")

        bad = os.path.join(td, "bad.cpp")
        with open(bad, "w") as f:
            f.write("class Broken { public: int x;\n")
        result = run(extractor, [bad])
        if result.returncode == 0:
            failures.append("a source file with syntax errors must fail")

        source = os.path.join(td, "fields.cpp")
        with open(source, "w") as f:
            f.write(
                "class Demo {\n"
                "public:\n"
                "    int value;\n"
                "    static int counter;\n"
                "    void set(int v);\n"
                "};\n"
                "void Demo::set(int v) { value = v; }\n"
                "int Demo::counter = 0;\n"
            )
        result = run(extractor, ["--format", "json", source])
        if result.returncode != 0:
            failures.append("valid source must succeed: " + result.stderr)
        else:
            report = json.loads(result.stdout)
            if report.get("debug_info") is not None:
                failures.append("source report debug_info must be null")
            cls = next(
                (c for c in report["classes"] if c["name"] == "Demo"), None
            )
            if cls is None:
                failures.append("class Demo is missing from the report")
            else:
                fields = {f["name"]: f for f in cls["fields"]}
                if "value" not in fields or fields["value"]["static"]:
                    failures.append("instance field 'value' is missing/wrong")
                if "counter" not in fields or not fields["counter"]["static"]:
                    failures.append("static field 'counter' is missing/wrong")
                methods = cls["methods"]
                signatures = {m["signature"] for m in methods}
                if "void set(int)" not in signatures:
                    failures.append(
                        "signature shape mismatch: " + repr(signatures)
                    )
                if not any(m.get("params") == "int" for m in methods):
                    failures.append("JSON method params field is missing")

        result = run(extractor, ["-Iinclude", source])
        if result.returncode == 0:
            failures.append("an unknown short option must fail")

        result = run(extractor, ["--output", "", source])
        if result.returncode == 0:
            failures.append("an empty --output value must fail")

        ir = os.path.join(td, "ir_input")
        with open(ir, "w") as f:
            f.write(
                "; a leading comment\n"
                "\n"
                "target datalayout = "
                '"e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"\n'
                'target triple = "x86_64-unknown-linux-gnu"\n'
                "\n"
                "define i32 @f() {\n"
                "  ret i32 0\n"
                "}\n"
            )
        result = run(extractor, ["--format", "text", ir])
        if result.returncode != 0 or "#input_type bitcode" not in result.stdout:
            failures.append(
                "textual IR after leading comments must be detected as bitcode: "
                + result.stdout
                + result.stderr
            )

        utf8 = os.path.join(td, "utf8_input")
        with open(utf8, "w", encoding="utf-8") as f:
            f.write("// 中文注释\nstruct U { int x; };\n")
        result = run(extractor, ["--format", "text", utf8])
        if result.returncode != 0 or "struct U" not in result.stdout:
            failures.append(
                "UTF-8 C++ source without extension must be accepted: "
                + result.stdout
                + result.stderr
            )

    if failures:
        for failure in failures:
            print("FAIL:", failure, file=sys.stderr)
        return 1
    print("class_extractor regression: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
