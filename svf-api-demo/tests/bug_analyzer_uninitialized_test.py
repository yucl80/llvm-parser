#!/usr/bin/env python3
"""Regression tests for uninitialized-read findings."""

from pathlib import Path

from bug_analyzer_test_support import analyze_fixture, parse_args


UNINITIALIZED = (
    "read_uninitialized_scalar",
    "read_uninitialized_wide_scalar",
    "read_uninitialized_bool",
    "read_uninitialized_float",
    "read_uninitialized_array_element",
    "read_uninitialized_struct_field",
    "read_uninitialized_nested_struct_field",
    "read_uninitialized_trailing_struct_field",
    "read_through_pointer_alias",
    "read_multiple_uninitialized_locals",
    "read_through_ambiguous_pointer",
    "read_before_pointer_escape",
    "read_dynamic_index_is_unresolved",
    "read_uninitialized_malloc_element",
    "read_uninitialized_new_scalar",
    "read_uninitialized_new_array_element",
    "read_uninitialized_aligned_alloc_element",
    "read_after_partial_memset_uninitialized_tail",
    "read_after_zero_length_memset",
    "read_after_dynamic_length_memset",
    "copy_uninitialized_source",
    "copy_partially_initialized_source",
    "copy_uninitialized_heap_source",
    "read_after_partial_branch",
    "read_after_loop_that_may_not_run",
    "read_after_partial_struct_assignment",
)

NO_FINDING = (
    "read_initialized_scalar",
    "read_initialized_array_element",
    "read_initialized_struct_field",
    "read_after_zero_initialized_struct",
    "read_after_whole_struct_assignment",
    "read_after_initialization_through_pointer_alias",
    "read_through_pointer_before_reassignment",
    "read_through_external_pointer",
    "read_initialized_malloc_element",
    "read_calloc_zero_initialized_element",
    "read_dynamic_sized_malloc_is_unresolved",
    "read_zero_sized_malloc_is_unresolved",
    "read_realloc_result_is_unresolved",
    "read_value_initialized_new_scalar",
    "read_value_initialized_new_array_element",
    "read_after_memset",
    "read_after_partial_memset",
    "read_after_memset_through_pointer_alias",
    "read_after_memcpy_from_initialized_source",
    "read_after_memmove_from_initialized_source",
    "read_after_memcpy_through_pointer_alias",
    "copy_zero_length_is_noop",
    "read_after_all_paths_initialize",
    "read_after_loop_with_initialized_value",
    "read_uninitialized_global",
    "read_function_parameter",
    "read_beyond_object_is_unresolved",
    "read_before_object_is_unresolved",
    "read_escaped_object_is_suppressed",
)

# function -> expected byte range in the UNINITIALIZED_READ message
EXPECTED_RANGES = {
    "read_uninitialized_scalar": "[0,4)",
    "read_uninitialized_wide_scalar": "[0,8)",
    "read_uninitialized_bool": "[0,1)",
    "read_uninitialized_array_element": "[2,3)",
    "read_uninitialized_struct_field": "[4,8)",
    "read_uninitialized_nested_struct_field": "[4,8)",
    "read_uninitialized_trailing_struct_field": "[8,12)",
    "read_uninitialized_malloc_element": "[4,8)",
    "read_uninitialized_new_array_element": "[8,12)",
    "read_after_partial_memset_uninitialized_tail": "[4,5)",
    "copy_uninitialized_source": "[0,4)",
    "copy_partially_initialized_source": "[0,4)",
}


def matches(finding, function):
    name = finding["function"]
    return name == function or name.startswith(function + "(")


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_uninitialized_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = [
            finding for finding in report["findings"]
            if finding["rule"] == "UNINITIALIZED_READ"
        ]
        assert findings, "no uninitialized-read findings reported"
        assert all(
            finding["cwe"] == "CWE-457" and
            finding["severity"] == "medium" and
            finding["category"] == "memory-safety"
            for finding in findings
        )

        for function in UNINITIALIZED:
            assert any(matches(finding, function) for finding in findings), (
                f"missing uninitialized-read finding: {function}")
        for function in NO_FINDING:
            assert not any(matches(finding, function) for finding in findings), (
                f"unexpected uninitialized-read finding: {function}")

        partial_copy = [
            finding for finding in findings
            if matches(finding, "read_initialized_prefix_after_partial_copy")
        ]
        assert len(partial_copy) == 1, (
            "expected only the uninitialized source-copy finding, not a "
            f"finding for the initialized destination byte: {partial_copy}")

        for function, byte_range in EXPECTED_RANGES.items():
            messages = [
                finding["message"] for finding in findings
                if matches(finding, function)
            ]
            assert messages and any(byte_range in message
                                    for message in messages), (
                f"expected range {byte_range} for {function}, got: {messages}")

        multi = [
            finding for finding in findings
            if matches(finding, "read_multiple_uninitialized_locals")
        ]
        assert len(multi) == 2, (
            f"expected two findings for two uninitialized locals, got: {multi}")

    print("All uninitialized-read analyzer tests passed.")


if __name__ == "__main__":
    main()
