#!/usr/bin/env python3
"""Regression tests for unsafe API findings."""

from pathlib import Path

from bug_analyzer_test_support import analyze_fixture, parse_args


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_unsafe_api_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = report["findings"]
        rules = {finding["rule"] for finding in findings}
        assert {"UNBOUNDED_COPY", "UNBOUNDED_INPUT", "INSECURE_TEMP_FILE",
                "UNTRUSTED_FORMAT_STRING", "SHELL_COMMAND", "UNBOUNDED_SCAN",
                "BUFFER_OVERFLOW", "FORMAT_SIZE_EXCEEDS_BUFFER"} <= rules

        unsafe_api_findings = [
            finding for finding in findings
            if finding["rule"] in {
                "UNBOUNDED_COPY", "UNBOUNDED_INPUT", "INSECURE_TEMP_FILE"
            }
        ]
        assert any("wide_copy" in finding["function"]
                   for finding in unsafe_api_findings)
        assert any("indirect_strcpy" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_COPY"
                   for finding in findings)
        assert any("undersized_literal_copy" in finding["function"]
                   for finding in unsafe_api_findings)
        assert any("wide_concat" in finding["function"]
                   for finding in unsafe_api_findings)
        assert any("narrow_concat" in finding["function"]
                   for finding in unsafe_api_findings)
        assert any("current_directory" in finding["function"]
                   for finding in unsafe_api_findings)
        assert any("insecure_temp_names" in finding["function"]
                   and finding["rule"] == "INSECURE_TEMP_FILE"
                   for finding in unsafe_api_findings)
        assert any("dynamic_format" in finding["function"]
                   and finding["rule"] == "UNTRUSTED_FORMAT_STRING"
                   for finding in findings)
        assert any("dynamic_file_format" in finding["function"]
                   and finding["rule"] == "UNTRUSTED_FORMAT_STRING"
                   for finding in findings)
        assert not any("namespaced_non_api_calls" in finding["function"]
                       and finding["rule"] in {
                           "SHELL_COMMAND", "UNTRUSTED_FORMAT_STRING"
                       }
                       for finding in findings)
        assert not any("constant_format" in finding["function"]
                       and finding["rule"] == "UNTRUSTED_FORMAT_STRING"
                       for finding in findings)
        assert not any("safe_local_literal_format" in finding["function"]
                       and finding["rule"] == "UNTRUSTED_FORMAT_STRING"
                       for finding in findings)
        assert not any("safe_local_literal_command" in finding["function"]
                       and finding["rule"] == "SHELL_COMMAND"
                       for finding in findings)
        assert not any("safe_local_literal_scan" in finding["function"]
                       and finding["rule"] == "UNBOUNDED_SCAN"
                       for finding in findings)
        assert any("dynamic_system_command" in finding["function"]
                   and finding["rule"] == "SHELL_COMMAND"
                   for finding in findings)
        assert any("dynamic_popen_command" in finding["function"]
                   and finding["rule"] == "SHELL_COMMAND"
                   for finding in findings)
        assert not any("fixed_system_command" in finding["function"]
                       and finding["rule"] == "SHELL_COMMAND"
                       for finding in findings)
        assert not any("fixed_popen_command" in finding["function"]
                       and finding["rule"] == "SHELL_COMMAND"
                       for finding in findings)

        for function in (
                "memcpy_overflow", "memmove_overflow", "read_overflow",
                "recv_overflow", "offset_memcpy_overflow",
                "memset_overflow", "fread_overflow",
                "dynamic_memcpy_length", "dynamic_read_length",
                "dynamic_fread_count", "memccpy_overflow",
                "oversized_dynamic_memcpy"):
            assert any(function in finding["function"]
                       and finding["rule"] == "BUFFER_OVERFLOW"
                       for finding in findings)
        assert any("oversized_snprintf" in finding["function"]
                   and finding["rule"] == "FORMAT_SIZE_EXCEEDS_BUFFER"
                   for finding in findings)
        assert not any("oversized_snprintf_size_but_short_output"
                       in finding["function"] and
                       finding["rule"] == "FORMAT_SIZE_EXCEEDS_BUFFER"
                       for finding in findings)
        assert any("dynamic_snprintf_size" in finding["function"]
                   and finding["rule"] == "FORMAT_SIZE_EXCEEDS_BUFFER"
                   for finding in findings)
        for function in (
                "bounded_dynamic_memcpy", "bounded_dynamic_read",
                "bounded_dynamic_fread", "bounded_dynamic_snprintf"):
            assert not any(function in finding["function"] and
                           finding["rule"] in {
                               "BUFFER_OVERFLOW",
                               "FORMAT_SIZE_EXCEEDS_BUFFER",
                           }
                           for finding in findings)
        assert not any("safe_fread" in finding["function"] and
                       finding["rule"] == "BUFFER_OVERFLOW"
                       for finding in findings)
        assert any("bounded_copy_with_oversized_count" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_COPY"
                   for finding in findings)
        assert any("bounded_concat_with_oversized_count" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_COPY"
                   for finding in findings)
        for function in ("safe_strncpy", "bounded_dynamic_strncpy"):
            assert not any(function in finding["function"] and
                           finding["rule"] == "UNBOUNDED_COPY"
                           for finding in findings)

        assert any("undersized_bounded_scan" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("dynamic_scan_format" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("undersized_wide_scan" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("undersized_positional_scan" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("undersized_sscanf" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("unbounded_fscanf" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("undersized_character_scan" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert any("positional_scan_input" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_SCAN"
                   for finding in findings)
        assert not any(finding["function"] == "bounded_scan"
                       for finding in findings)
        assert not any("safe_scanset_containing_percent" in finding["function"]
                       and finding["rule"] == "UNBOUNDED_SCAN"
                       for finding in findings)

        safe_functions = {
            "safe_literal_copy", "safe_local_literal_copy",
            "safe_strncpy", "bounded_dynamic_strncpy",
            "exact_fit_literal_copy", "exact_fit_memcpy",
            "safe_bounded_scan", "mixed_safe_scan_conversions",
            "safe_positional_scan", "safe_allocating_scan", "safe_sscanf",
            "oversized_snprintf_size_but_short_output",
            "safe_single_character_scan", "safe_snprintf", "safe_fscanf",
            "safe_fread",
        }
        unsafe_rules = {
            "UNBOUNDED_COPY", "UNBOUNDED_INPUT", "UNBOUNDED_SCAN",
            "INSECURE_TEMP_FILE", "UNTRUSTED_FORMAT_STRING", "SHELL_COMMAND",
        }
        assert not any(
            finding["function"] in safe_functions and
            finding["rule"] in unsafe_rules
            for finding in findings
        )

    print("All unsafe API analyzer tests passed.")


if __name__ == "__main__":
    main()
