#!/usr/bin/env python3

import csv
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))

from jam2test.results import (
    classify_metronome_clicks,
    match_clicks,
    mesh_collect_metrics,
    mesh_verdict,
    verdict_for,
)
from jam2test.metrics import normalized_pair_summary, summarize_csv
from jam2test.stress import _effective_stream_ms


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
    def test_asymmetric_pressure_uses_explicit_minimum_duration(self):
        self.assertEqual(18000, _effective_stream_ms({
            "minimum_stream_ms": 18000,
            "source_scenario": "asymmetric-safe-create-fast-join-pressure",
        }, 5000))

    def test_listener_compensated_verdict_requires_both_peer_contracts(self):
        result = normal_result("metronome-listener-compensated-metro-pulse")
        result["metrics"]["combined"].update({
            "grid_authority_consensus": True,
            "grid_revision_consensus": True,
            "grid_authority_states_sent_total": 1.0,
            "grid_authority_states_accepted_total": 1.0,
            "metronome_alignment_valid_sides": 2.0,
            "grid_authority_epoch_min": 1.0,
            "metronome_compensation_active_sides": 2.0,
        })
        result["metrics"]["server"]["grid_mode"] = 2
        result["metrics"]["client"]["grid_mode"] = 2
        result["metro_pulse_epoch_analysis"] = {"ok": True}
        passing = {"ok": True, "checks": {"click_near_average_remote_audio": True}}
        result["listener_compensated_pulse_analysis"] = {
            "ok": True,
            "combined": {"matched_pulses_min": 10},
            "contracts": {"server": passing, "client": passing},
        }
        self.assertEqual(verdict_for(result), "pass")

        result["listener_compensated_pulse_analysis"]["contracts"]["server"] = {
            "ok": False,
            "checks": {"click_near_average_remote_audio": False},
        }
        self.assertEqual(
            verdict_for(result),
            "listener_compensated_server_click_near_average_remote_audio")

    def test_catalog_duration_extends_past_last_native_action(self):
        scenario = {
            "source_scenario": "transport-track-sync-off",
            "commands": [{"at_s": 15.0}],
        }
        self.assertEqual(_effective_stream_ms(scenario, 8000), 17000)

    def test_requested_duration_remains_minimum_when_already_longer(self):
        scenario = {"source_scenario": "clean-control", "commands": []}
        self.assertEqual(_effective_stream_ms(scenario, 12000), 12000)

    def test_single_250ms_stall_retains_recovery_tail(self):
        scenario = {
            "source_scenario": "transient-stall-250-recovery",
            "impairment": type("Impairment", (), {
                "client_to_server": type("Direction", (), {
                    "burst_every_ms": 8000.0, "burst_pause_ms": 250.0})(),
                "server_to_client": type("Direction", (), {
                    "burst_every_ms": 8000.0, "burst_pause_ms": 250.0})(),
            })(),
            "commands": [],
        }
        self.assertEqual(_effective_stream_ms(scenario, 15000), 18000)

    def test_missing_first_click_is_a_startup_boundary(self):
        expected = [100, 1100, 2100]
        detected = [1105, 2104]
        result = classify_metronome_clicks(
            expected, detected, match_clicks(expected, detected))
        self.assertTrue(result["startup_boundary_mismatch"])
        self.assertEqual(result["steady_missing_clicks"], 0)
        self.assertEqual(result["steady_extra_clicks"], 0)

    def test_udp_flood_proves_bounded_observed_receive_batch(self):
        result = normal_result(
            "udp-short-flood",
            udp_parse_rejections_total=4096.0,
            udp_receive_batch_max=64.0)
        result["udp_validation"] = {
            "injections": [{"injected": 2048}, {"injected": 2048}],
        }
        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["udp_receive_batch_max"] = 65.0
        self.assertEqual(verdict_for(result), "udp_work_budget_exceeded")

    def test_benchmark_pair_summary_uses_normalized_machine_records(self):
        value = normalized_pair_summary("machine-a", None, "machine-b", None)
        self.assertEqual(["machine-a", "machine-b"],
                         [peer["machine_id"] for peer in value["peers"]])
        self.assertEqual({"coordinator", "agent"},
                         {peer["role"] for peer in value["peers"]})
        self.assertNotIn("server", value)
        self.assertNotIn("client", value)

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
            "playback_ring_underruns",
            "playback_ring_underrun_events",
            "playback_ring_underrun_time_ms",
            "playback_ring_underrun_burst_max_ms",
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
                "playback_ring_underruns": "0",
                "playback_ring_underrun_events": "0",
                "playback_ring_underrun_time_ms": "0",
                "playback_ring_underrun_burst_max_ms": "0",
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
                "playback_ring_underruns": "128",
                "playback_ring_underrun_events": "2",
                "playback_ring_underrun_time_ms": "2.66667",
                "playback_ring_underrun_burst_max_ms": "2.66667",
                "recv_packets": "1032",
            },
        ]

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            summary = summarize_csv(path, assessment_elapsed_ms=20000)

        self.assertEqual(summary["network_active_peer_count"], 0)
        self.assertEqual(summary["recv_packets"], 1032)
        self.assertEqual(summary["grid_mapped_epoch_frame"], 148200)
        self.assertEqual(summary["metronome_alignment_valid"], "yes")
        self.assertEqual(summary["metronome_epoch_sample_time"], 148200)
        self.assertEqual(summary["metronome_beat_delta_abs"], 0)
        self.assertEqual(summary["metronome_compensation_active"], "yes")
        self.assertEqual(summary["metronome_compensation_offset_frames"], 64)
        self.assertEqual(summary["playback_ring_underrun_time_ms"], 0)
        self.assertAlmostEqual(summary["playback_ring_underrun_time_ms_final"], 2.66667)

    def test_recovery_window_excludes_post_assessment_teardown(self):
        fields = [
            "row_type", "elapsed_ms", "adaptive_playback_padding_frames",
            "adaptive_playback_target_frames", "recv_packets", "mix_active_slots",
            "mix_max_slots", "playback_ring_readable_frames", "audio_control_playback_ratio",
        ]
        rows = [
            {"row_type": "periodic", "elapsed_ms": "12000",
             "adaptive_playback_padding_frames": "2624",
             "adaptive_playback_target_frames": "1291", "recv_packets": "1000",
             "mix_active_slots": "1", "mix_max_slots": "88",
             "playback_ring_readable_frames": "1290", "audio_control_playback_ratio": "1.005"},
            {"row_type": "periodic", "elapsed_ms": "14000",
             "adaptive_playback_padding_frames": "2624",
             "adaptive_playback_target_frames": "1150", "recv_packets": "2200",
             "mix_active_slots": "1", "mix_max_slots": "88",
             "playback_ring_readable_frames": "500", "audio_control_playback_ratio": "1.005"},
            {"row_type": "periodic", "elapsed_ms": "18000",
             "adaptive_playback_padding_frames": "2624",
             "adaptive_playback_target_frames": "1051", "recv_packets": "4000",
             "mix_active_slots": "1", "mix_max_slots": "88",
             "playback_ring_readable_frames": "400", "audio_control_playback_ratio": "1.005"},
            {"row_type": "periodic", "elapsed_ms": "19000",
             "adaptive_playback_padding_frames": "3648",
             "adaptive_playback_target_frames": "1536", "recv_packets": "4000",
             "mix_active_slots": "0", "mix_max_slots": "88",
             "playback_ring_readable_frames": "1536", "audio_control_playback_ratio": "1"},
            {"row_type": "final", "elapsed_ms": "19100",
             "adaptive_playback_padding_frames": "3648",
             "adaptive_playback_target_frames": "1536", "recv_packets": "4000",
             "mix_active_slots": "0", "mix_max_slots": "88",
             "playback_ring_readable_frames": "1536", "audio_control_playback_ratio": "1"},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            summary = summarize_csv(path, assessment_elapsed_ms=18000)
            self.assertEqual(summary["recovery_adaptive_padding_frames_delta"], 0.0)
            self.assertEqual(summary["adaptive_playback_target_observed_max"], 1291.0)
            self.assertEqual(summary["playback_ring_readable_recovery_max"], 500.0)
            self.assertEqual(summary["audio_control_playback_ratio_observed_max"], 1.005)


class NormalVerdictTests(unittest.TestCase):
    def test_malformed_verdict_tracks_generated_corpus_size(self):
        result = normal_result("malformed-udp", udp_parse_rejections_total=14.0)
        result["udp_validation"] = {
            "injections": [{"injected": True} for _ in range(14)],
        }

        self.assertEqual(verdict_for(result), "pass")

        result["udp_validation"]["injections"][-1]["injected"] = False
        self.assertEqual(verdict_for(result), "malformed_corpus_not_fully_injected")

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
            adaptive_target_recovered_frames_min=400.0,
            adaptive_min_frames_max=256.0,
            playback_ring_readable_recovery_max=320.0,
            playback_ring_recovered_frames_min=900.0,
            audio_control_playback_ratio_observed_max=1.005)
        result["requested_stream_ms"] = 18000
        result["metrics"]["combined"]["elapsed_s_min"] = 18.0
        result["proxy_stats"] = {
            "client_to_server_blackout_events": 1,
            "server_to_client_blackout_events": 1,
        }

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["recovery_mix_active_slots_ratio_end_max"] = 0.99
        self.assertEqual(verdict_for(result), "transient_stall_mixer_queue_not_recovered")
        result["metrics"]["combined"]["recovery_mix_active_slots_ratio_end_max"] = 0.02

        result["metrics"]["combined"]["audio_control_playback_ratio_observed_max"] = 1.0
        self.assertEqual(
            verdict_for(result), "transient_stall_adaptive_release_ratio_not_applied")
        result["metrics"]["combined"]["audio_control_playback_ratio_observed_max"] = 1.005

        result["metrics"]["combined"]["playback_ring_readable_recovery_max"] = 539.0
        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["playback_ring_readable_recovery_max"] = 700.0
        self.assertEqual(
            verdict_for(result), "transient_stall_playback_latency_not_recovered")

    def test_short_burst_requires_real_playback_latency_recovery(self):
        result = normal_result(
            "burst-pause-250",
            playback_underrun_time_ms_total=20.0,
            adaptive_raise_events_total=20.0,
            adaptive_min_frames_max=256.0,
            playback_ring_readable_recovery_max=320.0,
            audio_control_playback_ratio_observed_max=1.005)
        result["proxy_stats"] = {
            "client_to_server_blackout_events": 1,
            "server_to_client_blackout_events": 1,
        }

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["playback_ring_readable_recovery_max"] = 1500.0
        self.assertEqual(verdict_for(result), "burst_playback_latency_not_recovered")

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
            adaptive_target_recovered_frames_min=400.0,
            adaptive_min_frames_max=256.0,
            playback_ring_readable_recovery_max=320.0,
            playback_ring_recovered_frames_min=900.0,
            audio_control_playback_ratio_observed_max=1.005)
        result["requested_stream_ms"] = 18000
        result["metrics"]["combined"]["elapsed_s_min"] = 18.0
        result["proxy_stats"] = {
            "client_to_server_blackout_events": 1,
            "server_to_client_blackout_events": 1,
        }

        self.assertEqual(verdict_for(result), "transient_stall_adaptive_padding_not_recovered")

    def test_noop_metronome_controls_preserve_initial_grid(self):
        result = normal_result("grid-noop-running-controls")
        result["metrics"]["server"].update({
            "local_peer_id": 1,
            "grid_mode": 0,
            "grid_revision_before_shutdown": 1,
            "grid_authority_peer_id_before_shutdown": 1,
        })
        result["metrics"]["client"].update({
            "local_peer_id": 2,
            "grid_mode": 0,
            "grid_revision_before_shutdown": 1,
            "grid_authority_peer_id_before_shutdown": 1,
        })
        result["metrics"]["combined"].update({
            "grid_authority_consensus": True,
            "grid_revision_consensus": True,
            "grid_authority_epoch_min": 1000,
            "grid_mapped_epoch_min": 1000,
            "grid_authority_states_sent_total": 1,
            "grid_authority_states_accepted_total": 1,
            "metronome_alignment_valid_sides": 2,
            "grid_proposals_sent_total": 0,
        })

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["client"]["grid_revision_before_shutdown"] = 2
        self.assertEqual(verdict_for(result), "noop_controls_created_grid_revision")

    def test_concurrent_grid_edits_require_two_ordered_assignments_not_startup_revision(self):
        result = normal_result("grid-authority-concurrent")
        result["metrics"]["server"].update({
            "grid_mode": 0,
            "grid_revision": 2,
            "grid_authority_peer_id": 1,
        })
        result["metrics"]["client"].update({
            "grid_mode": 0,
            "grid_revision": 2,
            "grid_authority_peer_id": 1,
        })
        result["metrics"]["combined"].update({
            "grid_revision_consensus": True,
            "grid_authority_consensus": True,
            "grid_authority_states_sent_total": 2,
            "grid_authority_states_accepted_total": 2,
            "metronome_alignment_valid_sides": 2,
            "grid_authority_epoch_min": 1000,
            "grid_proposals_accepted_total": 2,
            "grid_assignments_accepted_total": 4,
        })

        self.assertEqual(verdict_for(result), "pass")

        result["metrics"]["combined"]["grid_assignments_accepted_total"] = 3
        self.assertEqual(verdict_for(result), "concurrent_grid_revisions_not_ordered")


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
