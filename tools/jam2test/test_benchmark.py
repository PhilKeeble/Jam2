import unittest
import tempfile
from pathlib import Path
from types import SimpleNamespace

from jam2test.benchmark import (
    _agent_attempt_status,
    _correlated_process_outcome,
    _format_case_validation,
    _write_peer_result,
)
from jam2test.format_comparison import format_comparison_summary


class BenchmarkOutcomeTests(unittest.TestCase):
    def test_correlated_attempt_completes_only_when_both_processes_succeed(self):
        self.assertEqual(("complete", True), _correlated_process_outcome(0, {"return_code": 0}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(4, {"return_code": 0}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(0, {"return_code": 4}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(0, {}))

    def test_agent_attempt_requires_upload_and_process_success(self):
        self.assertEqual("passed", _agent_attempt_status(True, 0))
        self.assertEqual("failed", _agent_attempt_status(True, 4))
        self.assertEqual("failed", _agent_attempt_status(False, 0))

    def test_missing_native_manifest_is_reported_without_masking_timeout(self):
        case = SimpleNamespace(
            signal="tone-440",
            coordinator_signal="tone-440",
            agent_signal="tone-440",
        )
        with tempfile.TemporaryDirectory() as folder:
            result = _write_peer_result(
                Path(folder), {"case_id": "case"}, "machine", "coordinator",
                return_code=-15, timed_out=True, case=case)
        self.assertTrue(result["timed_out"])
        self.assertEqual("", result["native_manifest"])
        self.assertIn("FileNotFoundError", result["native_manifest_error"])

    def test_matched_format_summary_reports_wire_bitrate_cpu_and_audio(self):
        def result(audio_format, packet_bytes, bitrate, cpu):
            return {
                "verdict": "complete",
                "case_id": f"fast_tone-440__{audio_format}",
                "run_index": 1,
                "metrics": {"aggregate": {
                    "udp_header_bytes": 36,
                    "audio_payload_bytes_max": packet_bytes - 36,
                    "audio_packet_bytes_max": packet_bytes,
                    "send_bitrate_bps_max": bitrate,
                    "recv_bitrate_bps_max": bitrate,
                    "loss_percent_max": 0.0,
                    "jitter_max_ms": 1.0,
                }},
                "peers": [{
                    "machine_id": "machine-a",
                    "role": "coordinator",
                    "performance": {
                        "process_cpu_time_ms": 100.0,
                        "process_cpu_percent_one_core": cpu,
                        "wall_elapsed_ms": 1000.0,
                    },
                    "analysis": {"ok": True, "stems": {"their-input": {
                        "rms": 0.1,
                        "peak": 0.25,
                        "pop_events": 0,
                        "clipped_frames": 0,
                    }}},
                }],
            }

        summary = format_comparison_summary([
            result("pcm16", 164, 904000, 3.0),
            result("pcm24", 228, 1256000, 4.0),
        ])
        self.assertEqual(1, summary["pair_count"])
        self.assertEqual(25.0, summary["wire_protocol"]["header_reduction_percent"])
        pair = summary["pairs"][0]
        self.assertAlmostEqual(
            (228 - 164) * 100.0 / 228,
            pair["pcm16_reduction_percent"]["audio_packet_bytes_max"])
        self.assertEqual(1, pair["formats"]["pcm16"]["non_silent_remote_wav_peers"])

    def test_matched_stress_summary_uses_combined_metrics_and_native_manifest(self):
        def result(audio_format, packet_bytes, bitrate, cpu):
            return {
                "verdict": "pass",
                "scenario": f"clean-control__{audio_format}",
                "metrics": {"combined": {
                    "udp_header_bytes": 36,
                    "audio_payload_bytes_max": packet_bytes - 36,
                    "audio_packet_bytes_max": packet_bytes,
                    "send_bitrate_bps_max": bitrate,
                    "recv_bitrate_bps_max": bitrate,
                }},
                "peers": [{
                    "native_manifest": {"result": {
                        "process_cpu_time_ms": 100.0,
                        "process_cpu_percent_one_core": cpu,
                        "wall_elapsed_ms": 1000.0,
                    }},
                }],
            }

        summary = format_comparison_summary([
            result("pcm16", 164, 904000, 3.0),
            result("pcm24", 228, 1256000, 4.0),
        ])
        self.assertEqual(1, summary["pair_count"])
        pair = summary["pairs"][0]
        self.assertEqual(164, pair["formats"]["pcm16"]["audio_packet_bytes_max"])
        self.assertEqual(3.0, pair["formats"]["pcm16"]["process_cpu_percent_one_core_mean"])

    def test_format_case_validation_requires_exact_format_packet_size_and_audio(self):
        case = SimpleNamespace(
            network_audio_format="pcm16",
            coordinator_signal="tone-440",
            agent_signal="tone-440",
        )
        result = {
            "metrics": {"aggregate": {
                "network_audio_formats": ["pcm16-mono"],
                "frame_size_max": 64.0,
                "network_audio_bytes_per_sample_max": 2,
                "audio_packet_bytes_max": 164,
                "sent_packets_min": 100,
                "recv_packets_min": 100,
                "send_bitrate_bps_max": 904000,
                "recv_bitrate_bps_max": 904000,
            }},
            "peers": [{"analysis": {"stems": {"their-input": {"peak": 0.25}}}}],
        }
        self.assertTrue(_format_case_validation(result, case)["ok"])
        result["metrics"]["aggregate"]["network_audio_formats"] = ["pcm24-mono"]
        self.assertFalse(_format_case_validation(result, case)["ok"])


if __name__ == "__main__":
    unittest.main()
