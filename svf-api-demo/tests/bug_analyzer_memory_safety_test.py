#!/usr/bin/env python3
"""Regression tests for memory-safety findings."""

from pathlib import Path

from bug_analyzer_test_support import analyze_fixture, parse_args


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_memory_safety_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = report["findings"]
        rules = {finding["rule"] for finding in findings}
        assert {"NULL_DEREFERENCE", "DIVISION_BY_ZERO", "BUFFER_OVERFLOW",
                "FORMAT_SIZE_EXCEEDS_BUFFER", "RETURN_STACK_ADDRESS",
                "OUT_OF_BOUNDS_ACCESS", "USE_AFTER_FREE", "DOUBLE_FREE"} <= rules

        assert any("memcpy_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        assert any("offset_memcpy_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        assert any("recv_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        assert any("snprintf_overflow" in finding["function"]
                   and finding["rule"] == "FORMAT_SIZE_EXCEEDS_BUFFER"
                   for finding in findings)
        assert any("return_stack_address" in finding["function"]
                   and finding["rule"] == "RETURN_STACK_ADDRESS"
                   for finding in findings)
        assert any("return_stack_array" in finding["function"]
                   and finding["rule"] == "RETURN_STACK_ADDRESS"
                   for finding in findings)
        for function in ("divide_by_zero", "remainder_by_zero"):
            assert any(function in finding["function"] and
                       finding["rule"] == "DIVISION_BY_ZERO"
                       for finding in findings)
        assert any("unbounded_literal_copy" in finding["function"]
                   and finding["rule"] == "UNBOUNDED_COPY"
                   for finding in findings)
        for function in (
                "out_of_bounds_read", "out_of_bounds_negative_index",
                "out_of_bounds_write", "out_of_bounds_nested_array",
                "out_of_bounds_struct_member", "out_of_bounds_pointer_read",
                "out_of_bounds_pointer_before_base",
                "out_of_bounds_pointer_write", "out_of_bounds_heap_read"):
            assert any(function in finding["function"] and
                       finding["rule"] == "OUT_OF_BOUNDS_ACCESS"
                       for finding in findings)
        for function in (
                "use_after_free_read", "use_after_free_write",
                "use_after_free_through_alias",
                "use_after_free_across_branch",
                "use_after_free_on_conditional_path",
                "use_after_free_atomic",
                "use_after_free_through_select",
                "return_freed_pointer"):
            assert any(function in finding["function"] and
                       finding["rule"] == "USE_AFTER_FREE"
                       for finding in findings)
        for function in ("out_of_bounds_atomic_rmw",
                         "out_of_bounds_atomic_cmpxchg"):
            assert any(function in finding["function"] and
                       finding["rule"] == "OUT_OF_BOUNDS_ACCESS"
                       for finding in findings)
        assert any("double_free_malloc" in finding["function"]
                   and finding["rule"] == "DOUBLE_FREE"
                   for finding in findings)
        assert any("double_free_across_branch" in finding["function"]
                   and finding["rule"] == "DOUBLE_FREE"
                   for finding in findings)
        assert any("double_delete" in finding["function"]
                   and finding["rule"] == "RESOURCE_DOUBLE_RELEASE"
                   for finding in findings)
        assert any("null_pointer_read" in finding["function"]
                   and finding["rule"] == "NULL_DEREFERENCE"
                   for finding in findings)
        assert any("null_pointer_write" in finding["function"]
                   and finding["rule"] == "NULL_DEREFERENCE"
                   for finding in findings)
        assert any("null_pointer_read_through_alias" in finding["function"]
                   and finding["rule"] == "NULL_DEREFERENCE"
                   for finding in findings)
        assert any("null_pointer_on_guarded_branch" in finding["function"]
                   and finding["rule"] == "NULL_DEREFERENCE"
                   for finding in findings)
        for function in ("null_pointer_read",
                         "null_pointer_read_through_alias"):
            assert not any(function in finding["function"] and
                           finding["rule"] == "OUT_OF_BOUNDS_ACCESS"
                           for finding in findings)
        assert any("memcpy_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        assert any("memmove_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        assert any("read_overflow" in finding["function"]
                   and finding["rule"] == "BUFFER_OVERFLOW"
                   for finding in findings)
        safe_functions = {
            "in_bounds_read", "in_bounds_write", "in_bounds_pointer_read",
            "in_bounds_negative_pointer_offset", "divide_by_input",
            "safe_malloc_after_null_check",
            "free_null_pointer", "matching_free",
            "namespaced_free_is_not_deallocator",
            "flexible_array_within_allocated_extent",
            "unreachable_out_of_bounds_label", "exact_fit_memcpy",
            "safe_snprintf", "safe_literal_copy",
        }
        unsafe_rules = {
            "NULL_DEREFERENCE", "DIVISION_BY_ZERO", "OUT_OF_BOUNDS_ACCESS",
            "RETURN_STACK_ADDRESS", "USE_AFTER_FREE", "DOUBLE_FREE",
            "BUFFER_OVERFLOW", "FORMAT_SIZE_EXCEEDS_BUFFER",
            "UNBOUNDED_COPY",
        }
        assert not any(
            finding["function"] in safe_functions and
            finding["rule"] in unsafe_rules
            for finding in findings
        )
        assert not any(
            finding["function"].startswith("divide_by_input")
            and finding["rule"] == "DIVISION_BY_ZERO"
            for finding in findings
        )

    print("All memory-safety analyzer tests passed.")


if __name__ == "__main__":
    main()
