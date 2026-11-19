#!/usr/bin/env python3
"""Regression tests for resource-lifetime findings."""

from pathlib import Path

from bug_analyzer_test_support import analyze_fixture, parse_args


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_resource_lifetime_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = report["findings"]
        resource_findings = [
            finding for finding in findings
            if finding["rule"].startswith("RESOURCE_")
        ]
        rules = {finding["rule"] for finding in resource_findings}
        assert {"RESOURCE_LEAK", "RESOURCE_MISMATCH",
                "RESOURCE_DOUBLE_RELEASE"} <= rules

        leaked_functions = {
            finding["function"] for finding in resource_findings
            if finding["rule"] == "RESOURCE_LEAK"
        }
        expected_leaks = (
            "malloc_leak", "calloc_leak", "strdup_leak",
            "realloc_null_result_leak",
            "reallocarray_leak", "scalar_new_leak", "array_new_leak",
            "conditional_path_leak", "early_return_leak",
            "overwrite_and_release", "overwrite_in_loop_then_release",
            "wrapped_resource_leak", "conditional_release_wrapper_leak",
            "wrapped_descriptor_leak", "file_stream_leak",
            "temporary_file_leak", "pipe_stream_leak",
            "file_descriptor_leak", "openat_descriptor_leak",
            "created_file_descriptor_leak", "socket_leak",
            "duplicated_descriptor_leak", "accepted_socket_leak",
            "mmap_leak", "directory_stream_leak", "dynamic_library_leak",
            "mkstemp_descriptor_leak", "indirect_acquisition_leak",
        )
        for function in expected_leaks:
            assert any(function in name for name in leaked_functions)
        assert sum(
            finding["rule"] == "RESOURCE_LEAK" and
            "overwrite_and_release" in finding["function"]
            for finding in resource_findings
        ) == 1
        assert not any(
            function in name for function in (
                "matching_malloc_free", "matching_new_delete",
                "matching_new_array_delete", "matching_file_close",
                "matching_temporary_file_close", "matching_pipe_close",
                "matching_descriptor_close", "matching_socket_close",
                "matching_fdopen_close", "unique_ptr_releases_allocation",
                "matching_release_wrapper",
                "matching_release_wrapper_with_branch",
                "release_each_loop_iteration", "checked_realloc_cleanup",
                "transfer_resource_to_global",
                "returned_resource_is_transferred", "return_resource",
                "matching_mmap_unmap", "matching_directory_close",
                "matching_dynamic_library_close",
                "indirect_release_matching",
                "cross_block_release_each_iteration",
            )
            for name in leaked_functions
        )

        mismatch_functions = {
            finding["function"] for finding in resource_findings
            if finding["rule"] == "RESOURCE_MISMATCH"
        }
        for function in (
                "malloc_delete_mismatch", "new_free_mismatch",
                "file_release_mismatch"):
            assert any(function in name for name in mismatch_functions)
        assert any("double_release" in finding["function"]
                   and finding["rule"] == "RESOURCE_DOUBLE_RELEASE"
                   for finding in resource_findings)

    print("All resource-lifetime analyzer tests passed.")


if __name__ == "__main__":
    main()
