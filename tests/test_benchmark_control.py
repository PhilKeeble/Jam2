#!/usr/bin/env python3

import sys
import socket
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from jam2_benchmark_control import (  # noqa: E402
    BenchmarkControlState,
    JsonLinePeer,
    control_endpoint_from_server_url,
    run_identity,
    same_run,
)


class BenchmarkControlTests(unittest.TestCase):
    def test_run_identity_normalizes_run_index(self):
        payload = {
            "suite_id": "suite",
            "case_id": "case",
            "run_index": "2",
            "attempt_id": "attempt",
        }
        self.assertEqual(run_identity(payload)["run_index"], 2)

    def test_same_run_rejects_stale_attempt(self):
        active = {
            "suite_id": "suite",
            "case_id": "case",
            "run_index": 1,
            "attempt_id": "new",
        }
        stale = {
            "suite_id": "suite",
            "case_id": "case",
            "run_index": 1,
            "attempt_id": "old",
        }
        self.assertFalse(same_run(stale, active))

    def test_control_endpoint_defaults_to_tcp_49000(self):
        self.assertEqual(
            control_endpoint_from_server_url("http://192.0.2.10:8000"),
            ("192.0.2.10", 49000),
        )
        self.assertEqual(
            control_endpoint_from_server_url("192.0.2.10"),
            ("192.0.2.10", 49000),
        )

    def test_state_tracks_case_messages_by_identity(self):
        state = BenchmarkControlState("suite", lambda _: None)
        case = {
            "suite_id": "suite",
            "case_id": "case",
            "run_index": 1,
            "attempt_id": "attempt",
        }
        state.publish_case(case)
        state.handle_message({"type": "case.accept", **case})
        self.assertIsNotNone(state.has_message("case.accept", case))
        self.assertIsNone(state.has_message("case.started", case))

    def test_json_line_peer_reads_binary_payload_after_header(self):
        left, right = socket.socketpair()
        try:
            sender = JsonLinePeer(left)
            receiver = JsonLinePeer(right)
            sender.send_with_payload({"type": "artifact.upload", "size": 4}, b"data")
            message = receiver.read()
            payload = receiver.read_exact(message["size"])
            self.assertEqual(message["type"], "artifact.upload")
            self.assertEqual(payload, b"data")
        finally:
            left.close()
            right.close()


if __name__ == "__main__":
    unittest.main()
