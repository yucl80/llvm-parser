#!/usr/bin/env python3
"""Regression tests for concurrency findings."""

from pathlib import Path

from bug_analyzer_test_support import analyze_fixture, parse_args


def main():
    analyzer, clangxx = parse_args()
    source = Path(__file__).with_name(
        "bug_analyzer_concurrency_fixture.cpp").read_text()
    with analyze_fixture(analyzer, clangxx, source) as (report, _, _):
        findings = report["findings"]
        rules = {finding["rule"] for finding in findings}
        assert {"POTENTIAL_DATA_RACE", "SELF_DEADLOCK",
                "UNLOCK_WITHOUT_LOCK", "UNBALANCED_UNLOCK",
                "LOCK_NOT_RELEASED", "LOCK_ORDER_CYCLE"} <= rules

        race_findings = [
            finding for finding in findings
            if finding["rule"] == "POTENTIAL_DATA_RACE"
        ]
        assert any("posix_write_write_race" in finding["message"]
                   for finding in race_findings)
        assert any("cpp_write_write_race" in finding["message"]
                   for finding in race_findings)
        assert any("async_race" in finding["message"]
                   for finding in race_findings)
        assert any("mixed_atomic" in finding["message"]
                   for finding in race_findings)
        assert any("beginthreadex_race" in finding["message"]
                   for finding in race_findings)
        assert not any("split_fields" in finding["message"]
                       for finding in race_findings)
        for global_name in (
                "posix_read_write_race", "mixed_api_race",
                "different_locks_race", "volatile_race"):
            assert any(global_name in finding["message"]
                       for finding in race_findings)
        assert not any(
            protected_name in finding["message"]
            for protected_name in (
                "posix_mutex_protected", "posix_rwlock_protected",
                "posix_spinlock_protected", "cpp_mutex_protected",
                "cpp_shared_mutex_protected", "cpp_lock_guard_protected",
                "cpp_unique_lock_protected", "atomic_counter",
                "thread_local_counter", "read_only_global",
                "wrapped_mutex_protected",
            )
            for finding in race_findings
        )
        assert not any(
            finding["rule"] == "SELF_DEADLOCK" and
            "cpp_recursive_self_lock" in finding["function"]
            for finding in findings
        )

        expected_functions = {
            "UNLOCK_WITHOUT_LOCK": "unlock_without_lock",
            "UNBALANCED_UNLOCK": "unbalanced_unlock",
            "LOCK_NOT_RELEASED": "lock_not_released",
            "SELF_DEADLOCK": "self_deadlock",
        }
        for rule, function in expected_functions.items():
            assert any(function in finding["function"] and
                       finding["rule"] == rule
                       for finding in findings)
        assert any("self_deadlock_with_lock_guards" in finding["function"] and
                   finding["rule"] == "SELF_DEADLOCK"
                   for finding in findings)
        cycle_functions = {
            finding["function"] for finding in findings
            if finding["rule"] == "LOCK_ORDER_CYCLE"
        }
        assert any("lock_order_ab" in name for name in cycle_functions)
        assert any("lock_order_ba" in name for name in cycle_functions)
        assert not any("consistent_lock_order" in name
                       for name in cycle_functions)

    print("All concurrency analyzer tests passed.")


if __name__ == "__main__":
    main()
