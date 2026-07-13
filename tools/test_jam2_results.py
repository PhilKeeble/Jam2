#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))

from jam2_results import mesh_collect_metrics, mesh_verdict, verdict_for


def normal_result(scenario="clean-control", **metric_overrides):
    metrics = {
        "has_csv": True,
        "elapsed_s_min": 10.0,
        "loss_percent_max": 0.0,
        "frame_size_max": 64.0,
        "audio_callbacks_min": 100.0,
    }
    metrics.update(metric_overrides)
    return {
        "source_scenario": scenario,
        "requested_stream_ms": 10000,
        "server_return_code": 0,
        "client_return_code": 0,
        "metrics": {
            "server": {"has_csv": True},
            "client": {"has_csv": True},
            "combined": metrics,
        },
    }


class NormalVerdictTests(unittest.TestCase):
    def test_clean_protocol_can_pass_while_audio_health_fails(self):
        result = normal_result(
            audio_callback_gap_over_2x_total=1.0,
            playback_underrun_time_ms_total=2.0)

        self.assertEqual(verdict_for(result), "audio_playback_underrun")
        self.assertEqual(result["protocol_verdict"], "pass")
        self.assertEqual(result["duration_verdict"], "pass")
        self.assertEqual(result["audio_health_verdict"], "audio_playback_underrun")
        self.assertEqual(result["audio_health_failures"], ["audio_playback_underrun"])
        self.assertEqual(result["audio_health_observations"], ["audio_callback_deadline_misses"])

    def test_clean_transport_and_audio_pass(self):
        result = normal_result()

        self.assertEqual(verdict_for(result), "pass")
        self.assertEqual(result["protocol_verdict"], "pass")
        self.assertEqual(result["audio_health_verdict"], "pass")
        self.assertEqual(result["audio_health_observations"], [])

    def test_forward_gap_allows_only_injected_packet_frames(self):
        result = normal_result(
            "forward-sequence-gap",
            udp_forward_gap_rejects_total=2.0,
            missing_audio_frames_total=128.0,
            jitter_buffer_dropped_frames_total=128.0)
        result["udp_validation"] = {
            "transformer": {"transformed_by_direction": {"a": 1, "b": 1}},
        }

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["missing_audio_frames_total"] = 129.0
        self.assertEqual(verdict_for(result), "audio_missing_frames")

    def test_short_effective_duration_fails_independently(self):
        result = normal_result(elapsed_s_min=4.0)

        self.assertEqual(verdict_for(result), "effective_duration_too_short")
        self.assertEqual(result["protocol_verdict"], "pass")
        self.assertEqual(result["duration_verdict"], "effective_duration_too_short")

    def test_both_process_stats_are_required(self):
        result = normal_result()
        result["metrics"]["client"]["has_csv"] = False

        self.assertEqual(verdict_for(result), "missing_csv")

    def test_forced_jitter_release_is_reported_without_calling_it_packet_loss(self):
        result = normal_result(jitter_buffer_forced_releases_total=1.0)

        self.assertEqual(verdict_for(result), "pass")
        self.assertEqual(result["protocol_verdict"], "pass")
        self.assertEqual(result["audio_health_failures"], [])
        self.assertEqual(result["audio_health_observations"], ["audio_jitter_forced_releases"])

    def test_impairment_case_does_not_apply_clean_audio_rules(self):
        result = normal_result(
            "loss-1.0",
            loss_percent_max=1.0,
            playback_dropped_frames_total=64000.0)
        result["proxy_stats"] = {"client_to_server_dropped": 1}

        self.assertEqual(verdict_for(result), "pass")
        self.assertEqual(result["audio_health_verdict"], "not_evaluated")


class HeadlessVerdictTests(unittest.TestCase):
    def test_recording_coverage_uses_requested_wall_clock_duration(self):
        peers = []
        for index in range(2):
            peers.append({
                "peer": f"peer{index + 1}",
                "return_code": 0,
                "csv_summary": {
                    "has_csv": True,
                    "active_sample_rate": 1000.0,
                    "elapsed_s": 1.0,
                    "sent_packets": 1.0,
                    "recv_packets": 1.0,
                    "audio_callbacks": 10.0,
                },
                "audio_analysis": {
                    "ok": True,
                    "tags": [],
                    "sidecar": {
                        "sample_rate": 1000,
                        "frames_written": 899,
                    },
                },
            })
        metrics = mesh_collect_metrics(peers, requested_stream_ms=1000)
        result = {
            "requested_stream_ms": 1000,
            "mesh_metrics": metrics,
        }

        self.assertEqual(metrics["recording_duration_coverage_ratio_min"], 0.899)
        self.assertEqual(mesh_verdict(result), "effective_duration_too_short")

    def test_stale_sample_rejection_is_not_a_clean_protocol_pass(self):
        result = {
            "requested_stream_ms": 1000,
            "mesh_metrics": {
                "peer_count": 2,
                "return_code_failures": 0,
                "peers_with_csv": 2,
                "sent_packets_min": 1.0,
                "recv_packets_min": 1.0,
                "stats_duration_coverage_ratio_min": 1.0,
                "recording_duration_coverage_ratio_min": 1.0,
                "audio_ok_peers": 2,
                "audio_callbacks_min": 10.0,
                "udp_sample_time_stale_rejects_total": 1.0,
            },
        }

        self.assertEqual(mesh_verdict(result), "unexpected_stale_sample_rejection")
        self.assertEqual(result["duration_verdict"], "pass")


if __name__ == "__main__":
    unittest.main()
