#!/usr/bin/env python3
"""Regression tests for nested-loop findings."""

import json
from pathlib import Path
import tempfile

from bug_analyzer_test_support import analyze_fixture, parse_args, run


def assert_small_fixed_trip_loops_are_filtered(analyzer, clangxx, source):
    with tempfile.TemporaryDirectory() as raw_temp_dir:
        temp_dir = Path(raw_temp_dir)
        bitcode = temp_dir / "fixture.bc"
        promoted_bitcode = temp_dir / "fixture-promoted.bc"
        run(str(clangxx), "-std=c++17", "-O0", "-Xclang",
            "-disable-O0-optnone", "-g", "-emit-llvm", "-c", str(source),
            "-o", str(bitcode))
        run(str(clangxx.with_name("opt")), "-passes=mem2reg", str(bitcode),
            "-o", str(promoted_bitcode))
        report = json.loads(run(
            str(analyzer), "--format", "json", str(promoted_bitcode)).stdout)
        findings = [
            finding["function"] for finding in report["findings"]
            if finding["rule"] == "NESTED_LOOPS"
        ]
        assert "small_fixed_trip_nested_loops()" not in findings
        assert any("large_fixed_trip_nested_loops" in name
                   for name in findings)
        assert any("small_fixed_trip_nested_loops_with_call" in name
                   for name in findings)


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_nested_loops_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = {
            finding["function"]: finding["message"]
            for finding in report["findings"]
            if finding["rule"] == "NESTED_LOOPS"
        }
        expected_depths = {
            "nested_for_loops": 2,
            "nested_while_loops": 2,
            "nested_do_while_loops": 2,
            "mixed_nested_loops": 3,
            "four_nested_loops": 4,
            "nested_loops_in_branches": 2,
            "sequential_nested_loop_pairs": 2,
            "nested_range_based_loops": 2,
            "small_fixed_trip_nested_loops": 2,
            "large_fixed_trip_nested_loops": 2,
            "small_fixed_trip_nested_loops_with_call": 2,
            "nested_loops_in_lambda": 2,
        }
        for function, depth in expected_depths.items():
            finding = next(
                (message for name, message in findings.items()
                 if function in name), None)
            assert finding is not None, f"missing nested-loop finding: {function}"
            assert f"depth: {depth}" in finding, (
                f"expected depth {depth} for {function}, got: {finding}")

        assert not any(
            function in name
            for function in ("sequential_loops", "loops_in_separate_branches")
            for name in findings
        )

    assert_small_fixed_trip_loops_are_filtered(
        analyzer, clangxx,
        Path(__file__).with_name("bug_analyzer_nested_loops_fixture.cpp"))

    print("All nested-loop analyzer tests passed.")


if __name__ == "__main__":
    main()
