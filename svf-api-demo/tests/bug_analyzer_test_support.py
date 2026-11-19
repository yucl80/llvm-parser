#!/usr/bin/env python3
"""Shared helpers for LLVM bitcode analyzer regression tests."""

import argparse
from contextlib import contextmanager
import json
import subprocess
import tempfile
from pathlib import Path


SOURCE = """\
#include <array>

int main() {
    const std::array<int, 4> samples{3, 5, 8, 13};
    int total = 0;
    for (int sample : samples)
        total += sample;
    return total == 0;
}
"""


def run(*args, ok=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if ok and result.returncode:
        raise RuntimeError("command failed:\n%s\n%s" %
                           (result.stdout, result.stderr))
    return result


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    parser.add_argument("--clangxx", required=True)
    args = parser.parse_args()
    return Path(args.analyzer), Path(args.clangxx)


@contextmanager
def analyze_fixture(analyzer, clangxx, source_text=SOURCE):
    with tempfile.TemporaryDirectory() as raw_temp_dir:
        temp_dir = Path(raw_temp_dir)
        source = temp_dir / "fixture.cpp"
        bitcode = temp_dir / "fixture.bc"
        source.write_text(source_text)
        run(str(clangxx), "-std=c++17", "-O0", "-g", "-emit-llvm", "-c",
            "-Wno-div-by-zero", str(source), "-o", str(bitcode))

        report = json.loads(run(
            str(analyzer), "--format", "json", str(bitcode)).stdout)
        assert all(finding["input"] == str(bitcode)
                   for finding in report["findings"])
        yield report, bitcode, temp_dir
