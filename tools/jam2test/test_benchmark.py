import json
import unittest
import tempfile
import zipfile
from pathlib import Path
from types import SimpleNamespace

from jam2test.benchmark import (
    _apply_wav_retention,
    _agent_attempt_status,
    _coverage_class,
    _correlated_process_outcome,
    _extract_upload,
    _flatten_attempt_artifacts,
    _format_case_validation,
    _network_outlier_assessment,
    _package,
    _scenario,
    _write_peer_result,
)
from jam2test.benchmark_suite import CATALOG_VERSION, benchmark_cases
from jam2test.format_comparison import format_comparison_summary
from jam2test.profiles import configure_native_profiles


class BenchmarkOutcomeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configure_native_profiles({"profiles": [
            {
                "name": "fast", "frame_size": 64, "audio_buffer_size": 32,
                "playback_prefill_frames": 256, "playout_delay_frames": 256,
                "playback_ring_frames": 4096, "playback_max_frames": 1536,
                "adaptive_playback_max_frames": 1536,
                "jitter_buffer_frames": 512,
            },
            {
                "name": "moderate", "frame_size": 128, "audio_buffer_size": 64,
                "playback_prefill_frames": 512, "playout_delay_frames": 512,
                "playback_ring_frames": 8192, "playback_max_frames": 4096,
                "adaptive_playback_max_frames": 4096,
                "jitter_buffer_frames": 2048,
            },
            {
                "name": "safe", "frame_size": 256, "audio_buffer_size": 64,
                "playback_prefill_frames": 1024, "playout_delay_frames": 1024,
                "playback_ring_frames": 8192, "playback_max_frames": 7168,
                "adaptive_playback_max_frames": 7168,
                "jitter_buffer_frames": 2048,
            },
        ]})

    def test_fixed_catalog_has_stable_core_prefix_and_single_primary_runs(self):
        core = benchmark_cases("core")
        full = benchmark_cases("full")
        self.assertEqual(1, CATALOG_VERSION)
        self.assertEqual(9, len(core))
        self.assertEqual(25, len(full))
        self.assertEqual(
            [case.case_id for case in core],
            [case.case_id for case in full[:len(core)]])
        self.assertEqual("control-fast-tone-pcm24", core[0].case_id)
        self.assertEqual(len(full), len({case.case_id for case in full}))
        self.assertTrue(all(case.repeats == 1 for case in full))
        known = {case.case_id for case in full}
        self.assertTrue(all(case.comparator in known for case in full))

    def test_headless_scenario_inherits_profile_callback_and_variant_override(self):
        control = benchmark_cases("core")[0]
        callback = next(
            case for case in benchmark_cases("full")
            if case.case_id == "callback-fast-128-frame-64-tone-pcm24")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            common = (
                "network.create", "run", root, None, 48000,
                "0.0.0.0:49001", "invite", "session", "key",
                "127.0.0.1:49001", "coordinator", "machine",
            )
            control_runtime = _scenario(control, *common)["runtime"]
            callback_runtime = _scenario(callback, *common)["runtime"]
        self.assertTrue(control_runtime["headless_audio"])
        self.assertNotIn("audio_buffer_size", control_runtime)
        self.assertEqual(128, callback_runtime["audio_buffer_size"])
        self.assertEqual(
            40_000,
            _scenario(control, *common)["network"]["wait_ms"])

    def test_coverage_classes_are_explicit(self):
        self.assertEqual(
            "headless-headless",
            _coverage_class([{"audio_mode": "headless"}, {"audio_mode": "headless"}]))
        self.assertEqual(
            "physical-physical",
            _coverage_class([{"audio_mode": "physical"}, {"audio_mode": "physical"}]))
        self.assertEqual(
            "mixed",
            _coverage_class([{"audio_mode": "headless"}, {"audio_mode": "physical"}]))

    def test_network_outlier_thresholds_are_bounded_and_objective(self):
        baseline = {
            "rtt_avg_ms_max": 20.0,
            "jitter_avg_ms_max": 2.0,
            "loss_percent_max": 0.0,
        }
        normal = {
            "rtt_avg_ms_max": 25.0,
            "jitter_avg_ms_max": 4.0,
            "loss_percent_max": 0.1,
        }
        outlier = dict(normal, rtt_avg_ms_max=31.0)
        self.assertFalse(_network_outlier_assessment(baseline, normal)["triggered"])
        self.assertTrue(_network_outlier_assessment(baseline, outlier)["triggered"])

    def test_wavs_are_deleted_only_after_complete_analysis(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            recording = root / "recording"
            recording.mkdir()
            for stem in ("mix", "my-input", "their-input", "inputs-mix", "metronome"):
                (recording / f"{stem}.wav").write_bytes(b"wav")
            retained = _apply_wav_retention(root, {"analysis_complete": False})
            self.assertEqual(5, len(retained["retained"]))
            deleted = _apply_wav_retention(root, {"analysis_complete": True})
            self.assertEqual(5, len(deleted["deleted"]))
            self.assertEqual([], deleted["retained"])

    def test_uploaded_agent_files_share_execution_folder_with_prefixes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "source"
            target = root / "execution"
            source.mkdir(parents=True)
            peer = {
                "csv_path": "stats.csv",
                "native_manifest": "native-manifest.json",
                "recording_analysis": "recording-analysis.json",
                "analysis": {"stems": {"mix": {"path": "mix.wav"}}},
                "wav_retention": {"retained": ["mix.wav"]},
            }
            (source / "peer-result.json").write_text(
                json.dumps(peer), encoding="utf-8")
            (source / "stats.csv").write_text("value\n1\n", encoding="utf-8")
            (source / "recording-analysis.json").write_text(
                json.dumps({"stems": {"mix": {"path": "mix.wav"}}}),
                encoding="utf-8")
            (source / "native-manifest.json").write_text(
                json.dumps({"artifacts": [{"path": "stats.csv"}]}),
                encoding="utf-8")
            (source / "mix.wav").write_bytes(b"wav")
            archive = root / "agent.zip"
            with zipfile.ZipFile(archive, "w") as output:
                for path in source.rglob("*"):
                    if path.is_file():
                        output.write(path, path.relative_to(source).as_posix())

            rewritten = _extract_upload(archive, target)

            self.assertTrue((target / "agent_peer-result.json").is_file())
            self.assertTrue((target / "agent_stats.csv").is_file())
            self.assertTrue((target / "agent_mix.wav").is_file())
            self.assertEqual("agent_stats.csv", rewritten["csv_path"])
            self.assertEqual(
                "agent_mix.wav",
                rewritten["analysis"]["stems"]["mix"]["path"])

    def test_native_csv_and_recording_evidence_are_flattened_after_analysis(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "csv").mkdir()
            (root / "recording").mkdir()
            (root / "csv" / "stats.csv").write_text(
                "value\n1\n", encoding="utf-8")
            (root / "recording" / "recording.json").write_text(
                "{}", encoding="utf-8")
            (root / "native-manifest.json").write_text(
                "{}", encoding="utf-8")
            native = {"artifacts": [
                {"path": "csv/stats.csv"},
                {"path": "recording/recording.json"},
                {"path": "recording/mix.wav", "sha256": "retained-analysis-hash"},
            ]}
            analysis = {"recording_dir": str(root / "recording")}

            moves = _flatten_attempt_artifacts(root, native, analysis)

            self.assertTrue((root / "stats.csv").is_file())
            self.assertTrue((root / "recording.json").is_file())
            self.assertFalse((root / "csv").exists())
            self.assertFalse((root / "recording").exists())
            self.assertEqual(str(root), analysis["recording_dir"])
            self.assertEqual("stats.csv", moves["csv/stats.csv"])
            self.assertEqual(
                ["stats.csv", "recording.json", "mix.wav"],
                [artifact["path"] for artifact in native["artifacts"]])

    def test_package_scrubs_control_secrets_and_keeps_incomplete_audio(self):
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder)
            source = parent / "invocation"
            attempt = source / "attempt"
            recording = attempt / "recording"
            recording.mkdir(parents=True)
            result = {
                "suite_id": "suite", "benchmark_suite": "core",
                "benchmark_catalog_version": 1,
                "case_id": "control-fast-tone-pcm24",
                "run_index": 1, "run_kind": "primary",
                "attempt_id": "attempt", "attempt_number": 1,
                "verdict": "complete", "coverage_class": "headless-headless",
                "case": {
                    "case_id": "control-fast-tone-pcm24",
                    "category": "control",
                    "comparator": "control-fast-tone-pcm24",
                    "network_audio_format": "pcm24",
                    "signal": "tone-440",
                    "profile": {"profile": "fast", "base_profile": "fast"},
                },
                "control": {"invite": "jam2://secret", "session_key": "secret"},
                "metrics": {"aggregate": {"rtt_avg_ms_max": 1.0}, "peers": []},
                "peers": [{
                    "machine_id": "a", "role": "coordinator",
                    "audio_mode": "headless",
                    "effective_configuration": {"headless_audio": True},
                    "analysis": {"analysis_complete": False, "ok": False,
                                 "recording_dir": str(recording), "stems": {}},
                    "performance": {},
                    "build": {"executable": "C:\\private\\jam2.exe", "sha256": "abc"},
                }],
            }
            (attempt / "correlated-result.json").write_text(
                json.dumps(result), encoding="utf-8")
            (attempt / "peer-result.json").write_text(
                json.dumps(result["peers"][0]), encoding="utf-8")
            (attempt / "stats.csv").write_text("field\nvalue\n", encoding="utf-8")
            (recording / "mix.wav").write_bytes(b"failed-analysis-audio")
            target = parent / "submission.zip"
            self.assertEqual(
                0, _package(SimpleNamespace(results=source, output=target)))
            with zipfile.ZipFile(target) as archive:
                names = set(archive.namelist())
                analysis = archive.read("analysis.json").decode("utf-8")
            self.assertIn("submission-manifest.json", names)
            self.assertTrue(any(name.endswith("mix.wav") for name in names))
            self.assertNotIn("jam2://secret", analysis)
            self.assertNotIn("C:\\\\private\\\\jam2.exe", analysis)

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
