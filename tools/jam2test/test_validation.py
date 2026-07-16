import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from jam2test.validation import (
    ValidationReporter,
    _listener_compensation_contract,
    _mesh_edge_summaries,
    _startup_events,
)


class ValidationReportingTests(unittest.TestCase):
    def test_reporter_emits_only_case_and_summary_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                reporter = ValidationReporter(1, root)
                reporter.start("example")
                reporter.finish("example", True, "peers=4")
                reporter.summary(True)
            lines = output.getvalue().splitlines()
            self.assertEqual(lines[0], "[RUN ] 01/01 example")
            self.assertRegex(lines[1], r"^\[PASS\] 01/01 example \(\d+\.\ds\) peers=4$")
            self.assertRegex(lines[2], r"^\[SUMMARY\] PASS 1/1 \(\d+\.\ds\) artifacts=")
            self.assertEqual(len(lines), 3)

    def test_setup_failure_is_distinct_from_case_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                reporter = ValidationReporter(1, root)
                reporter.fail_setup("missing executable")
                reporter.summary(False, "missing executable")
            lines = output.getvalue().splitlines()
            self.assertTrue(lines[0].startswith("[FAIL] setup reason=missing executable artifacts="))
            self.assertTrue(lines[1].startswith("[SUMMARY] INFRASTRUCTURE-ERROR artifacts="))

    def test_public_startup_and_active_edge_output_are_parsed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stdout.log"
            path.write_text(
                "noise\n"
                '{"event":"startup","mode":"join","stage":"connecting"}\n'
                'worker update={"event":"startup","mode":"join","stage":"connected"}\n'
                "mesh_peer endpoint=127.0.0.1:49000 peer_id=7 "
                "endpoint_proof_state=active sent_packets=100 recv_packets=99\n",
                encoding="utf-8",
            )
            self.assertEqual(
                [item["stage"] for item in _startup_events(path)],
                ["connecting", "connected"],
            )
            edges = _mesh_edge_summaries(path)
            self.assertEqual(set(edges), {"7"})
            self.assertTrue(edges["7"]["endpoint_proof_verified"])
            self.assertEqual(edges["7"]["sent_packets"], 100)
            self.assertEqual(edges["7"]["recv_packets"], 99)

    def test_listener_compensation_contract_requires_all_peers_and_average_target(self):
        csv_summary = {
            "active_sample_rate": 48000.0,
            "metronome_compensation_active": "yes",
            "metronome_compensation_base_frames": 100.0,
            "metronome_compensation_target_frames": -860.0,
            "metronome_compensation_offset_frames": -850.0,
            "metronome_compensation_average_latency_frames": 960.0,
            "metronome_compensation_peer_count": 3.0,
        }
        audio = {
            "listener_compensated_pulse": {
                "steady_avg_abs_error_ms": 2.0,
                "steady_max_abs_error_ms": 4.0,
            }
        }
        result = _listener_compensation_contract(csv_summary, audio, 3)
        self.assertTrue(result["ok"])
        self.assertTrue(result["checks"]["all_remote_peers_averaged"])
        self.assertTrue(result["checks"]["target_matches_average_latency"])

        csv_summary["metronome_compensation_peer_count"] = 1.0
        result = _listener_compensation_contract(csv_summary, audio, 3)
        self.assertFalse(result["ok"])
        self.assertFalse(result["checks"]["all_remote_peers_averaged"])


if __name__ == "__main__":
    unittest.main()
