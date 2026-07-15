#!/usr/bin/env python3

import csv
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))

from jam2_results import mesh_collect_metrics, mesh_verdict, verdict_for
from jam2_metrics import summarize_csv


def normal_result(scenario="clean-control", **metric_overrides):
    metrics = {
        "has_csv": True,
        "elapsed_s_min": 10.0,
        "loss_percent_max": 0.0,
        "frame_size_max": 64.0,
        "audio_callbacks_min": 100.0,
        "peer_identity_valid": True,
        "session_contract_valid": True,
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


class CsvSummaryTests(unittest.TestCase):
    def test_alignment_uses_last_periodic_row_before_shutdown(self):
        fields = [
            "row_type",
            "elapsed_ms",
            "network_active_peer_count",
            "grid_mapped_epoch_frame",
            "grid_mapping_error_frames",
            "metronome_alignment_valid",
            "metronome_epoch_sample_time",
            "local_metronome_beat",
            "remote_metronome_beat",
            "metronome_compensation_active",
            "metronome_compensation_offset_frames",
            "metronome_compensation_offset_ms",
            "metronome_compensation_target_frames",
            "metronome_compensation_target_ms",
            "recv_packets",
        ]
        rows = [
            {
                "row_type": "periodic",
                "elapsed_ms": "20000",
                "network_active_peer_count": "1",
                "grid_mapped_epoch_frame": "148200",
                "grid_mapping_error_frames": "0",
                "metronome_alignment_valid": "yes",
                "metronome_epoch_sample_time": "148200",
                "local_metronome_beat": "3",
                "remote_metronome_beat": "3",
                "metronome_compensation_active": "yes",
                "metronome_compensation_offset_frames": "64",
                "metronome_compensation_offset_ms": "1.451247",
                "metronome_compensation_target_frames": "64",
                "metronome_compensation_target_ms": "1.451247",
                "recv_packets": "1000",
            },
            {
                "row_type": "final",
                "elapsed_ms": "20634",
                "network_active_peer_count": "0",
                "grid_mapped_epoch_frame": "0",
                "grid_mapping_error_frames": "0",
                "metronome_alignment_valid": "no",
                "metronome_epoch_sample_time": "0",
                "local_metronome_beat": "0",
                "remote_metronome_beat": "0",
                "metronome_compensation_active": "no",
                "metronome_compensation_offset_frames": "0",
                "metronome_compensation_offset_ms": "0",
                "metronome_compensation_target_frames": "0",
                "metronome_compensation_target_ms": "0",
                "recv_packets": "1032",
            },
        ]

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            summary = summarize_csv(path)

        self.assertEqual(summary["network_active_peer_count"], 0)
        self.assertEqual(summary["recv_packets"], 1032)
        self.assertEqual(summary["grid_mapped_epoch_frame"], 148200)
        self.assertEqual(summary["metronome_alignment_valid"], "yes")
        self.assertEqual(summary["metronome_epoch_sample_time"], 148200)
        self.assertEqual(summary["metronome_beat_delta_abs"], 0)
        self.assertEqual(summary["metronome_compensation_active"], "yes")
        self.assertEqual(summary["metronome_compensation_offset_frames"], 64)


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

    def test_transient_stall_requires_bounded_recovery(self):
        result = normal_result(
            "transient-stall-recovery",
            jitter_max_ms=120.0,
            frame_size_max=64.0,
            recovery_window_ms_min=5000.0,
            recovery_recv_packets_delta_min=3000.0,
            mix_capacity_drops_total=0.0,
            recovery_mix_capacity_drops_delta_total=0.0,
            recovery_mix_active_slots_ratio_max=0.99,
            recovery_mix_active_slots_ratio_end_max=0.02,
            recovery_adaptive_padding_frames_delta_total=0.0,
            adaptive_raise_events_total=40.0,
            adaptive_release_events_total=100.0,
            adaptive_target_recovered_frames_min=400.0)
        result["requested_stream_ms"] = 18000
        result["metrics"]["combined"]["elapsed_s_min"] = 18.0
        result["proxy_stats"] = {
            "client_to_server_blackout_events": 1,
            "server_to_client_blackout_events": 1,
        }

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["recovery_mix_active_slots_ratio_end_max"] = 0.99
        self.assertEqual(verdict_for(result), "transient_stall_mixer_queue_not_recovered")

    def test_transient_stall_rejects_continuing_padding(self):
        result = normal_result(
            "transient-stall-recovery",
            jitter_max_ms=120.0,
            frame_size_max=64.0,
            recovery_window_ms_min=5000.0,
            recovery_recv_packets_delta_min=3000.0,
            recovery_mix_active_slots_ratio_max=0.02,
            recovery_mix_active_slots_ratio_end_max=0.02,
            recovery_adaptive_padding_frames_delta_total=9000.0,
            adaptive_raise_events_total=40.0,
            adaptive_release_events_total=100.0,
            adaptive_target_recovered_frames_min=400.0)
        result["requested_stream_ms"] = 18000
        result["metrics"]["combined"]["elapsed_s_min"] = 18.0
        result["proxy_stats"] = {
            "client_to_server_blackout_events": 1,
            "server_to_client_blackout_events": 1,
        }

        self.assertEqual(verdict_for(result), "transient_stall_adaptive_padding_not_recovered")


class HeadlessVerdictTests(unittest.TestCase):
    def test_mesh_teardown_uses_established_membership_high_water(self):
        peers = []
        for index in range(3):
            peers.append({
                "peer": f"peer{index + 1}",
                "return_code": 0,
                "csv_summary": {
                    "has_csv": True,
                    "active_sample_rate": 1000.0,
                    "elapsed_s": 1.0,
                    "sent_packets": 100.0,
                    "recv_packets": 100.0,
                    "audio_callbacks": 10.0,
                    "local_peer_id": index + 1,
                    "network_peer_count": 1 if index == 0 else 2,
                    "network_active_peer_count": 1 if index == 0 else 2,
                    "mix_contributing_peers": 1 if index == 0 else 2,
                    "network_peer_count_observed_max": 2,
                    "network_active_peer_count_observed_max": 2,
                    "mix_contributing_peers_observed_max": 2,
                    "mix_released_slots": 10,
                    "mix_complete_slots": 10,
                    "mix_output_frames": 100,
                    "grid_authority_peer_id": 1,
                    "bootstrap_coordinator_peer_id": 1,
                    "grid_revision": 1,
                    "grid_authority_states_sent": 1,
                    "grid_authority_states_accepted": 1,
                },
                "audio_analysis": {
                    "ok": True,
                    "tags": [],
                    "sidecar": {"sample_rate": 1000, "frames_written": 1000},
                },
            })
        result = {
            "requested_stream_ms": 1000,
            "peer_results": peers,
            "mesh_metrics": mesh_collect_metrics(peers, requested_stream_ms=1000),
        }

        self.assertEqual(result["mesh_metrics"]["network_peer_count_min"], 1)
        self.assertEqual(result["mesh_metrics"]["network_peer_count_established_min"], 2)
        self.assertEqual(mesh_verdict(result), "pass")

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
                    "local_peer_id": index + 1,
                    "network_peer_count": 1,
                    "network_active_peer_count": 1,
                    "mix_contributing_peers": 1,
                    "mix_released_slots": 1,
                    "mix_complete_slots": 1,
                    "mix_output_frames": 1,
                    "grid_authority_peer_id": 1,
                    "bootstrap_coordinator_peer_id": 1,
                    "grid_revision": 1,
                    "grid_authority_states_sent": 1,
                    "grid_authority_states_accepted": 1,
                    "drift_ppm": 180.0 if index == 0 else -160.0,
                    "resampler_ratio": 1.00018 if index == 0 else 0.99984,
                    "drift_correction_active_percent": 0.0,
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
        self.assertEqual(metrics["drift_correction_active_peers"], 2)
        self.assertAlmostEqual(metrics["resampler_ratio_delta_max"], 0.00018)
        self.assertEqual(mesh_verdict(result), "effective_duration_too_short")

    def test_stale_sample_rejection_is_not_a_clean_protocol_pass(self):
        result = {
            "requested_stream_ms": 1000,
            "mesh_metrics": {
                "peer_count": 2,
                "return_code_failures": 0,
                "peers_with_csv": 2,
                "expected_remote_peers": 1,
                "distinct_local_peer_ids": 2,
                "network_peer_count_min": 1,
                "network_peer_count_max": 1,
                "network_active_peer_count_min": 1,
                "network_active_peer_count_max": 1,
                "mix_contributing_peers_min": 1,
                "mix_released_slots_min": 1.0,
                "mix_complete_slots_min": 1.0,
                "mix_output_frames_min": 1.0,
                "grid_authority_peer_ids": [1],
                "grid_revision_min": 1,
                "grid_revision_max": 1,
                "grid_authority_states_sent_total": 1.0,
                "grid_authority_states_accepted_total": 1.0,
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
