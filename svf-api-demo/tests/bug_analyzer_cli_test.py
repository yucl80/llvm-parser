#!/usr/bin/env python3
"""Regression tests for analyzer report and CLI behavior."""

from bug_analyzer_test_support import analyze_fixture, parse_args, run


def main():
    analyzer, clangxx = parse_args()
    with analyze_fixture(analyzer, clangxx) as (report, bitcode, temp_dir):
        assert all(finding["line"] > 0 for finding in report["findings"])
        assert all(finding["file"] for finding in report["findings"])
        assert all(finding["start_line"] > 0
                   for finding in report["findings"])
        assert all(finding["end_line"] >= finding["start_line"]
                   for finding in report["findings"])
        assert "manual validation" in report["analysis"]

        invalid = run(str(analyzer), "--format", "xml", str(bitcode), ok=False)
        assert invalid.returncode != 0

        malformed = temp_dir / "invalid.bc"
        malformed.write_text("not LLVM IR")
        invalid = run(str(analyzer), str(malformed), ok=False)
        assert invalid.returncode != 0
        assert "[ERROR]" in invalid.stderr

    print("All analyzer CLI tests passed.")


if __name__ == "__main__":
    main()
