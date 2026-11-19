#!/usr/bin/env python3
"""Regression tests for source-level network calls inside loop bodies."""

import argparse
import json
import subprocess
from pathlib import Path


def run(analyzer, *args, ok=True):
    result = subprocess.run([str(analyzer), *map(str, args)],
                           capture_output=True, text=True, timeout=30)
    if ok and result.returncode:
        raise RuntimeError("analyzer failed: %s\n%s" %
                           (result.stdout, result.stderr))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    analyzer = Path(parser.parse_args().analyzer)
    source = Path(__file__).with_name(
        "bug_analyzer_network_loop_fixture.cpp")
    report = json.loads(run(analyzer, "--format", "json", source).stdout)
    findings = report["findings"]
    assert report["has_looped_network_calls"]

    expected_apis = {
        ("connect_with_retries", "connect"),
        ("send_request_fully", "send"),
        ("receive_response", "recv"),
        ("receive_response_until_closed", "recv"),
        ("receive_in_for_condition", "recv"),
        ("send_in_for_increment", "send"),
        ("complete_tls_handshake", "SSL_connect"),
        ("write_tls_request_fully", "SSL_write"),
        ("queue_asio_requests", "boost::asio::async_write"),
        ("write_http_requests", "boost::beast::http::write"),
        ("send_via_local_function_pointer", "send"),
        ("send_via_initialized_function_pointer", "send"),
        ("send_via_callback_parameter", "send"),
        ("register_network_callback_in_loop", "send"),
        ("invoke_callback_in_loop", "send"),
    }
    for function, api in expected_apis:
        matching = [
            item for item in findings
            if item["function"] == function and api in item["network_api"]
        ]
        assert matching, f"missing {api} finding in {function}"

    nested = [
        item for item in findings
        if item["function"] == "connect_with_retries"
    ]
    assert len(nested) == 2, (
        f"expected findings for both nested connection loops, got {nested}")
    assert any(item["function"] == "connect_to_asio_endpoints" and
               "connect" in item["network_api"]
               for item in findings)

    helper = next(
        item for item in findings
        if item["function"] == "write_http_requests")
    assert helper["call_chain"][:2] == [
        "write_http_requests", "write_http_request"]
    assert "boost::beast::http::write" in helper["call_chain"][-1]

    assert not any(item["function"] == "send_once_outside_loop"
                   for item in findings)
    assert not any(item["function"] == "local_send_name_in_loop"
                    for item in findings)
    assert not any(item["function"] == "never_run_network_calls"
                    for item in findings)
    assert any(item["function"] == "send_via_callback_parameter" and
               item["network_api"] == "send"
               for item in findings)

    invalid = run(analyzer, "--format", "xml", source, ok=False)
    assert invalid.returncode != 0

    print("All network_loop_analyzer tests passed.")


if __name__ == "__main__":
    main()
