"""Result processing and offline audio verdict helpers for Jam2 scenarios."""

import json
import math
import time
import wave
from pathlib import Path

from jam2_audio_analysis import analyze_metronome_wav, analyze_recording_dir
from jam2_scenarios import audio_probe_health_ok


METRONOME_WAV_TOLERANCE_FRAMES = 96
LISTENER_PULSE_STEADY_SKIP = 8
MIN_DURATION_COVERAGE_RATIO = 0.90
STRICT_AUDIO_HEALTH_SCENARIOS = {
    "clean-control",
    "duplicate-2.0",
    "delayed-replay",
    "forward-sequence-gap",
    "extreme-sample-time",
    "udp-short-flood",
}


def _recording_coverage(peer, requested_stream_ms):
    if requested_stream_ms <= 0:
        return 1.0
    audio = peer.get("audio_analysis", {})
    sidecar = audio.get("sidecar", {})
    csv_summary = peer.get("csv_summary", {})
    sample_rate = float(sidecar.get("sample_rate", 0) or csv_summary.get("active_sample_rate", 0.0))
    frames_written = float(sidecar.get("frames_written", 0))
    if frames_written <= 0.0:
        stem_lengths = [
            float(stem.get("frames", 0))
            for stem in audio.get("stems", {}).values()
            if stem.get("exists", False)
        ]
        frames_written = min(stem_lengths, default=0.0)
    expected_frames = sample_rate * requested_stream_ms / 1000.0
    return frames_written / expected_frames if expected_frames > 0.0 else 0.0


def mesh_collect_metrics(peer_results, requested_stream_ms=0):
    peers_with_csv = [peer for peer in peer_results if peer.get("csv_summary", {}).get("has_csv")]
    audio_tags = []
    for peer in peer_results:
        audio = peer.get("audio_analysis", {})
        audio_tags.extend(audio.get("tags", []))
    summaries = [peer["csv_summary"] for peer in peers_with_csv]
    recording_coverages = [_recording_coverage(peer, requested_stream_ms) for peer in peer_results]
    requested_s = requested_stream_ms / 1000.0
    expected_remote_peers = max(0, len(peer_results) - 1)
    return {
        "has_csv": bool(summaries),
        "peer_count": len(peer_results),
        "peers_with_csv": len(peers_with_csv),
        "expected_remote_peers": expected_remote_peers,
        "distinct_local_peer_ids": len({
            row.get("local_peer_id", 0) for row in summaries if row.get("local_peer_id", 0) != 0
        }),
        "grid_authority_peer_ids": sorted({
            row.get("grid_authority_peer_id", 0)
            for row in summaries
            if row.get("grid_authority_peer_id", 0) != 0
        }),
        "bootstrap_coordinator_peer_ids": sorted({
            row.get("bootstrap_coordinator_peer_id", 0)
            for row in summaries
            if row.get("bootstrap_coordinator_peer_id", 0) != 0
        }),
        "grid_revision_min": min((row.get("grid_revision", 0) for row in summaries), default=0),
        "grid_revision_max": max((row.get("grid_revision", 0) for row in summaries), default=0),
        "grid_authority_states_sent_total": sum(
            (row.get("grid_authority_states_sent", 0.0) for row in summaries), 0.0),
        "grid_authority_states_accepted_total": sum(
            (row.get("grid_authority_states_accepted", 0.0) for row in summaries), 0.0),
        "grid_proposals_sent_total": sum(
            (row.get("grid_proposals_sent", 0.0) for row in summaries), 0.0),
        "leader_audio_injected_peers": sum(
            (1 for row in summaries if row.get("leader_audio_injected_packets", 0.0) > 0.0), 0),
        "leader_audio_source_peer_ids": sorted({
            row.get("leader_audio_source_peer_id", 0)
            for row in summaries
            if row.get("leader_audio_injected_packets", 0.0) > 0.0
        }),
        "network_peer_count_min": min((row.get("network_peer_count", 0) for row in summaries), default=0),
        "network_peer_count_max": max((row.get("network_peer_count", 0) for row in summaries), default=0),
        "network_active_peer_count_min": min(
            (row.get("network_active_peer_count", 0) for row in summaries), default=0),
        "network_active_peer_count_max": max(
            (row.get("network_active_peer_count", 0) for row in summaries), default=0),
        "network_peer_count_established_min": min(
            (row.get("network_peer_count_observed_max", row.get("network_peer_count", 0))
             for row in summaries), default=0),
        "network_peer_count_established_max": max(
            (row.get("network_peer_count_observed_max", row.get("network_peer_count", 0))
             for row in summaries), default=0),
        "network_active_peer_count_established_min": min(
            (row.get("network_active_peer_count_observed_max", row.get("network_active_peer_count", 0))
             for row in summaries), default=0),
        "network_active_peer_count_established_max": max(
            (row.get("network_active_peer_count_observed_max", row.get("network_active_peer_count", 0))
             for row in summaries), default=0),
        "mix_contributing_peers_min": min(
            (row.get("mix_contributing_peers", 0) for row in summaries), default=0),
        "mix_contributing_peers_established_min": min(
            (row.get("mix_contributing_peers_observed_max", row.get("mix_contributing_peers", 0))
             for row in summaries), default=0),
        "mix_released_slots_min": min((row.get("mix_released_slots", 0.0) for row in summaries), default=0.0),
        "mix_complete_slots_min": min((row.get("mix_complete_slots", 0.0) for row in summaries), default=0.0),
        "mix_deadline_slots_total": sum((row.get("mix_deadline_slots", 0.0) for row in summaries), 0.0),
        "mix_missing_peer_frames_total": sum(
            (row.get("mix_missing_peer_frames", 0.0) for row in summaries), 0.0),
        "mix_late_after_release_frames_total": sum(
            (row.get("mix_late_after_release_frames", 0.0) for row in summaries), 0.0),
        "mix_capacity_drops_total": sum((row.get("mix_capacity_drops", 0.0) for row in summaries), 0.0),
        "mix_capacity_dropped_frames_total": sum(
            (row.get("mix_capacity_dropped_frames", 0.0) for row in summaries), 0.0),
        "mix_output_frames_min": min((row.get("mix_output_frames", 0.0) for row in summaries), default=0.0),
        "mix_output_drop_requested_frames_total": sum(
            (row.get("mix_output_drop_requested_frames", 0.0) for row in summaries), 0.0),
        "mix_output_drop_request_events_total": sum(
            (row.get("mix_output_drop_request_events", 0.0) for row in summaries), 0.0),
        "mix_output_dropped_frames_total": sum(
            (row.get("mix_output_dropped_frames", 0.0) for row in summaries), 0.0),
        "mix_clipped_samples_total": sum((row.get("mix_clipped_samples", 0.0) for row in summaries), 0.0),
        "return_code_failures": sum(1 for peer in peer_results if peer.get("return_code") != 0),
        "elapsed_s_min": min((row.get("elapsed_s", 0.0) for row in summaries), default=0.0),
        "stats_duration_coverage_ratio_min": (
            min((row.get("elapsed_s", 0.0) / requested_s for row in summaries), default=0.0)
            if requested_s > 0.0 else 1.0),
        "recording_duration_coverage_ratio_min": min(recording_coverages, default=0.0),
        "sent_packets_min": min((row.get("sent_packets", 0.0) for row in summaries), default=0.0),
        "recv_packets_min": min((row.get("recv_packets", 0.0) for row in summaries), default=0.0),
        "sent_packets_total": sum((row.get("sent_packets", 0.0) for row in summaries), 0.0),
        "recv_packets_total": sum((row.get("recv_packets", 0.0) for row in summaries), 0.0),
        "sequence_lost_total": sum((row.get("sequence_lost", 0.0) for row in summaries), 0.0),
        "sequence_loss_percent_max": max((row.get("sequence_loss_percent", 0.0) for row in summaries), default=0.0),
        "sequence_out_of_order_total": sum((row.get("sequence_out_of_order", 0.0) for row in summaries), 0.0),
        "sequence_duplicate_total": sum((row.get("sequence_duplicate", 0.0) for row in summaries), 0.0),
        "jitter_max_ms": max((row.get("jitter_max_ms", 0.0) for row in summaries), default=0.0),
        "rtt_max_ms": max((row.get("rtt_max_ms", 0.0) for row in summaries), default=0.0),
        "drift_abs_ppm_max": max((abs(row.get("drift_ppm", 0.0)) for row in summaries), default=0.0),
        "drift_correction_active_peers": sum(
            1 for row in summaries
            if (row.get("drift_correction_active_percent", 0.0) > 0.0 or
                abs(row.get("resampler_ratio", 1.0) - 1.0) > 0.000001)),
        "resampler_ratio_delta_max": max((
            max(
                abs(row.get("resampler_ratio", 1.0) - 1.0),
                abs(row.get("resampler_ratio_min", 1.0) - 1.0)
                if row.get("resampler_ratio_min", 0.0) > 0.0 else 0.0,
                abs(row.get("resampler_ratio_max", 1.0) - 1.0)
                if row.get("resampler_ratio_max", 0.0) > 0.0 else 0.0)
            for row in summaries
        ), default=0.0),
        "playback_underrun_time_ms_total": sum((row.get("playback_ring_underrun_time_ms", 0.0) for row in summaries), 0.0),
        "playback_overruns_total": sum((row.get("playback_ring_overruns", 0.0) for row in summaries), 0.0),
        "playback_dropped_frames_total": sum((row.get("playback_dropped_frames", 0.0) for row in summaries), 0.0),
        "missing_audio_frames_total": sum((row.get("missing_audio_frames_inserted", 0.0) for row in summaries), 0.0),
        "late_audio_frames_total": sum((row.get("late_audio_frames_dropped", 0.0) for row in summaries), 0.0),
        "jitter_buffer_dropped_frames_total": sum(
            (row.get("jitter_buffer_dropped_frames", 0.0) for row in summaries), 0.0),
        "jitter_buffer_forced_releases_total": sum(
            (row.get("jitter_buffer_forced_releases", 0.0) for row in summaries), 0.0),
        "audio_callbacks_min": min((row.get("audio_callbacks", 0.0) for row in summaries), default=0.0),
        "audio_callback_interval_avg_ms_max": max(
            (row.get("audio_callback_interval_avg_ms", 0.0) for row in summaries), default=0.0),
        "audio_callback_gap_over_2x_total": sum(
            (row.get("audio_callback_gap_over_2x_count", 0.0) for row in summaries), 0.0),
        "udp_sample_time_stale_rejects_total": sum(
            (row.get("udp_sample_time_stale_rejects", 0.0) for row in summaries), 0.0),
        "udp_sample_time_future_rejects_total": sum(
            (row.get("udp_sample_time_future_rejects", 0.0) for row in summaries), 0.0),
        "reorder_capacity_drops_total": sum((row.get("reorder_capacity_drops", 0.0) for row in summaries), 0.0),
        "jitter_capacity_drops_total": sum((row.get("jitter_capacity_drops", 0.0) for row in summaries), 0.0),
        "audio_ok_peers": sum(1 for peer in peer_results if peer.get("audio_analysis", {}).get("ok", False)),
        "audio_tags": sorted(set(audio_tags)),
    }


def mesh_verdict(result):
    metrics = result.get("mesh_metrics", {})
    peer_count = metrics.get("peer_count", 0)
    protocol_verdict = "pass"
    if metrics.get("return_code_failures", 0) > 0:
        protocol_verdict = "process_failed"
    elif metrics.get("peers_with_csv", 0) != peer_count:
        protocol_verdict = "missing_csv"
    elif metrics.get("distinct_local_peer_ids", 0) != peer_count:
        protocol_verdict = "mesh_peer_identity_invalid"
    elif (metrics.get("network_peer_count_established_min", metrics.get("network_peer_count_min", -1)) !=
          metrics.get("expected_remote_peers", 0) or
          metrics.get("network_peer_count_established_max", metrics.get("network_peer_count_max", -1)) !=
          metrics.get("expected_remote_peers", 0)):
        protocol_verdict = "mesh_peer_count_mismatch"
    elif (metrics.get("network_active_peer_count_established_min", metrics.get("network_active_peer_count_min", -1)) !=
          metrics.get("expected_remote_peers", 0) or
          metrics.get("network_active_peer_count_established_max", metrics.get("network_active_peer_count_max", -1)) !=
          metrics.get("expected_remote_peers", 0)):
        protocol_verdict = "mesh_endpoint_proof_incomplete"
    elif metrics.get("mix_contributing_peers_established_min", metrics.get("mix_contributing_peers_min", -1)) != \
            metrics.get("expected_remote_peers", 0):
        protocol_verdict = "mesh_mixer_contributors_missing"
    elif metrics.get("mix_released_slots_min", 0.0) <= 0.0 or metrics.get("mix_output_frames_min", 0.0) <= 0.0:
        protocol_verdict = "mesh_mixer_output_missing"
    elif metrics.get("mix_complete_slots_min", 0.0) <= 0.0:
        protocol_verdict = "mesh_mixer_complete_slots_missing"
    elif metrics.get("mix_capacity_drops_total", 0.0) > 0.0:
        protocol_verdict = "mesh_mixer_capacity_drops"
    elif metrics.get("mix_output_dropped_frames_total", 0.0) > 0.0:
        protocol_verdict = "mesh_mixer_output_drops"
    elif (len(set(result.get("headless_clock_drift_ppm", []))) > 1 and
          metrics.get("drift_abs_ppm_max", 0.0) < 50.0):
        protocol_verdict = "headless_clock_drift_not_observed"
    elif (len(set(result.get("headless_clock_drift_ppm", []))) > 1 and
          metrics.get("drift_correction_active_peers", 0) <= 0):
        protocol_verdict = "headless_clock_drift_not_corrected"
    elif (result.get("edge_impairments") and
          len(result.get("edge_proxy_stats", [])) != len(result.get("edge_impairments", []))):
        protocol_verdict = "mesh_edge_proxy_missing"
    elif (result.get("edge_impairments") and any(
            edge.get("stats", {}).get("client_to_server_packets", 0) <= 0 or
            edge.get("stats", {}).get("server_to_client_packets", 0) <= 0 or
            (edge.get("stats", {}).get("client_to_server_delayed", 0) +
             edge.get("stats", {}).get("server_to_client_delayed", 0)) <= 0
            for edge in result.get("edge_proxy_stats", []))):
        protocol_verdict = "mesh_edge_impairment_not_exercised"
    elif metrics.get("sent_packets_min", 0.0) <= 0.0 or metrics.get("recv_packets_min", 0.0) <= 0.0:
        protocol_verdict = "mesh_packets_missing"
    elif len(metrics.get("grid_authority_peer_ids", [])) != 1:
        protocol_verdict = "mesh_grid_authority_conflict"
    elif metrics.get("grid_revision_min", 0) <= 0 or metrics.get("grid_revision_min") != metrics.get("grid_revision_max"):
        protocol_verdict = "mesh_grid_revision_conflict"
    elif metrics.get("grid_authority_states_sent_total", 0.0) <= 0.0:
        protocol_verdict = "mesh_grid_authority_state_missing"
    elif metrics.get("grid_authority_states_accepted_total", 0.0) <= 0.0:
        protocol_verdict = "mesh_grid_authority_state_not_accepted"
    elif metrics.get("sequence_lost_total", 0.0) > 0.0:
        protocol_verdict = "unexpected_loss"
    elif metrics.get("sequence_out_of_order_total", 0.0) > 0.0:
        protocol_verdict = "unexpected_reorder"
    elif metrics.get("udp_sample_time_stale_rejects_total", 0.0) > 0.0:
        protocol_verdict = "unexpected_stale_sample_rejection"
    elif metrics.get("udp_sample_time_future_rejects_total", 0.0) > 0.0:
        protocol_verdict = "unexpected_future_sample_rejection"

    if protocol_verdict == "pass" and result.get("authority_peer"):
        expected = next((
            peer.get("csv_summary", {}).get("local_peer_id", 0)
            for peer in result.get("peer_results", [])
            if peer.get("peer") == result.get("authority_peer")), 0)
        actual = metrics.get("grid_authority_peer_ids", [])
        if expected <= 0 or actual != [expected]:
            protocol_verdict = "mesh_requested_authority_not_applied"
        elif metrics.get("grid_revision_min", 0) < 2:
            protocol_verdict = "mesh_authority_revision_not_applied"
        elif (expected not in metrics.get("bootstrap_coordinator_peer_ids", []) and
              metrics.get("grid_proposals_sent_total", 0.0) <= 0.0):
            protocol_verdict = "mesh_authority_proposal_not_sent"
        elif metrics.get("leader_audio_injected_peers", 0) != 1:
            protocol_verdict = "mesh_leader_audio_source_count_invalid"
        elif metrics.get("leader_audio_source_peer_ids", []) != [expected]:
            protocol_verdict = "mesh_leader_audio_source_not_authority"

    duration_verdict = duration_verdict_for(result)
    audio_health_failures = []
    audio_health_observations = []
    if metrics.get("audio_callback_gap_over_2x_total", 0.0) > 0.0:
        audio_health_observations.append("audio_callback_deadline_misses")
    if metrics.get("jitter_buffer_forced_releases_total", 0.0) > 0.0:
        audio_health_observations.append("audio_jitter_forced_releases")
    if metrics.get("playback_underrun_time_ms_total", 0.0) > 0.0:
        audio_health_observations.append("audio_playback_startup_underrun")
    if metrics.get("audio_ok_peers", 0) != peer_count:
        audio_health_failures.append("mesh_audio_probe_failed")
    if metrics.get("audio_callbacks_min", 0.0) <= 0.0:
        audio_health_failures.append("audio_callbacks_missing")
    if metrics.get("reorder_capacity_drops_total", 0.0) > 0.0:
        audio_health_failures.append("audio_reorder_capacity_drops")
    if metrics.get("jitter_capacity_drops_total", 0.0) > 0.0:
        audio_health_failures.append("audio_jitter_capacity_drops")
    if metrics.get("playback_overruns_total", 0.0) > 0.0:
        audio_health_failures.append("audio_playback_overruns")
    if metrics.get("playback_dropped_frames_total", 0.0) > 0.0:
        audio_health_failures.append("audio_playback_frames_dropped")
    if metrics.get("missing_audio_frames_total", 0.0) > 0.0:
        audio_health_failures.append("audio_missing_frames")
    if metrics.get("late_audio_frames_total", 0.0) > 0.0:
        audio_health_failures.append("audio_late_frames")
    if metrics.get("jitter_buffer_dropped_frames_total", 0.0) > 0.0:
        audio_health_failures.append("audio_jitter_frames_dropped")
    if metrics.get("playback_underrun_time_ms_total", 0.0) > max(250.0, peer_count * 250.0):
        audio_health_failures.append("audio_playback_underrun")
    audio_health_verdict = audio_health_failures[0] if audio_health_failures else "pass"

    result["protocol_verdict"] = protocol_verdict
    result["duration_verdict"] = duration_verdict
    result["audio_health_verdict"] = audio_health_verdict
    result["audio_health_failures"] = audio_health_failures
    result["audio_health_observations"] = audio_health_observations
    result["duration_coverage_ratio_min"] = duration_coverage_ratio(result)
    if protocol_verdict != "pass":
        return protocol_verdict
    if duration_verdict != "pass":
        return duration_verdict
    if audio_health_verdict != "pass":
        return audio_health_verdict
    return "pass"


def duration_coverage_ratio(result):
    requested_stream_ms = float(result.get("requested_stream_ms", 0))
    if requested_stream_ms <= 0.0:
        return 1.0
    if "mesh_metrics" in result:
        metrics = result.get("mesh_metrics", {})
        return min(
            metrics.get("stats_duration_coverage_ratio_min", 0.0),
            metrics.get("recording_duration_coverage_ratio_min", 0.0))
    elapsed_s = result.get("metrics", {}).get("combined", {}).get("elapsed_s_min", 0.0)
    return elapsed_s / (requested_stream_ms / 1000.0)


def duration_verdict_for(result):
    if duration_coverage_ratio(result) < MIN_DURATION_COVERAGE_RATIO:
        return "effective_duration_too_short"
    return "pass"


def audio_health_failures_for(result):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario not in STRICT_AUDIO_HEALTH_SCENARIOS:
        return []
    metrics = result.get("metrics", {}).get("combined", {})
    allowed_missing_frames = 0.0
    if scenario in {"forward-sequence-gap", "extreme-sample-time"}:
        allowed_missing_frames = 2.0 * metrics.get("frame_size_max", 0.0)
    failures = []
    if metrics.get("audio_callbacks_min", 0.0) <= 0.0:
        failures.append("audio_callbacks_missing")
    if metrics.get("reorder_capacity_drops_total", 0.0) > 0.0:
        failures.append("audio_reorder_capacity_drops")
    if metrics.get("jitter_capacity_drops_total", 0.0) > 0.0:
        failures.append("audio_jitter_capacity_drops")
    if metrics.get("playback_overruns_total", 0.0) > 0.0:
        failures.append("audio_playback_overruns")
    if metrics.get("playback_dropped_frames_total", 0.0) > 0.0:
        failures.append("audio_playback_frames_dropped")
    if metrics.get("missing_audio_frames_total", 0.0) > allowed_missing_frames:
        failures.append("audio_missing_frames")
    if metrics.get("late_audio_frames_total", 0.0) > 0.0:
        failures.append("audio_late_frames")
    if metrics.get("jitter_buffer_dropped_frames_total", 0.0) > allowed_missing_frames:
        failures.append("audio_jitter_frames_dropped")
    if metrics.get("playback_underrun_time_ms_total", 0.0) > 0.0:
        failures.append("audio_playback_underrun")
    return failures


def audio_health_observations_for(result):
    metrics = result.get("metrics", {}).get("combined", {})
    observations = []
    if metrics.get("audio_callback_gap_over_2x_total", 0.0) > 0.0:
        observations.append("audio_callback_deadline_misses")
    if metrics.get("jitter_buffer_forced_releases_total", 0.0) > 0.0:
        observations.append("audio_jitter_forced_releases")
    return observations


def audio_health_verdict_for(result):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario not in STRICT_AUDIO_HEALTH_SCENARIOS:
        return "not_evaluated"
    failures = audio_health_failures_for(result)
    return failures[0] if failures else "pass"




def protocol_verdict_for(result):
    metric_set = result.get("metrics", {})
    metrics = metric_set.get("combined", {})
    proxy = result.get("proxy_stats", {})
    if result.get("server_return_code") != 0 or result.get("client_return_code") != 0:
        return "process_failed"
    if (
            not metrics.get("has_csv")
            or not metric_set.get("server", {}).get("has_csv")
            or not metric_set.get("client", {}).get("has_csv")):
        return "missing_csv"
    if not metrics.get("peer_identity_valid", False):
        return "network_peer_identity_invalid"
    if not metrics.get("session_contract_valid", False):
        return "network_session_contract_invalid"
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario == "clean-control":
        if metrics.get("loss_percent_max", 0.0) > 0.0:
            return "unexpected_loss"
        return "pass"
    if scenario.startswith("loss-"):
        injected = proxy.get("client_to_server_dropped", 0) + proxy.get("server_to_client_dropped", 0)
        if injected <= 0:
            return "no_proxy_loss_injected"
        if metrics.get("loss_percent_max", 0.0) <= 0.0 and metrics.get("missing_audio_frames_total", 0.0) <= 0.0:
            return "loss_not_observed"
        return "pass"
    if scenario.startswith("jitter-"):
        if metrics.get("jitter_max_ms", 0.0) <= 0.0 and metrics.get("rtt_max_ms", 0.0) <= 0.0:
            return "impairment_not_visible"
        if proxy.get("client_to_server_dropped", 0) + proxy.get("server_to_client_dropped", 0) > 0:
            return "unexpected_proxy_drop"
        return "pass"
    if scenario.startswith("burst-pause-"):
        blackouts = proxy.get("client_to_server_blackout_events", 0) + proxy.get("server_to_client_blackout_events", 0)
        if blackouts <= 0:
            return "burst_blackout_not_injected"
        if (
                metrics.get("playback_underrun_time_ms_total", 0.0) <= 0.0
                and metrics.get("missing_audio_frames_total", 0.0) <= 0.0
                and metrics.get("sequence_lost_total", 0.0) <= 0.0):
            return "burst_not_observed"
        return "pass"
    if scenario == "transient-stall-recovery":
        blackouts = proxy.get("client_to_server_blackout_events", 0) + proxy.get("server_to_client_blackout_events", 0)
        if blackouts != 2:
            return "transient_stall_not_injected_once_per_direction"
        if metrics.get("jitter_max_ms", 0.0) < 50.0:
            return "transient_stall_not_observed"
        if metrics.get("recovery_window_ms_min", 0.0) < 4000.0:
            return "transient_stall_recovery_window_too_short"
        if metrics.get("recovery_recv_packets_delta_min", 0.0) <= 0.0:
            return "transient_stall_packet_flow_not_recovered"
        if metrics.get("mix_capacity_drops_total", 0.0) > 0.0:
            return "transient_stall_mixer_capacity_drops"
        if metrics.get("recovery_mix_capacity_drops_delta_total", 0.0) > 0.0:
            return "transient_stall_mixer_still_dropping"
        if metrics.get("recovery_mix_active_slots_ratio_max", 0.0) > 0.25:
            return "transient_stall_mixer_queue_not_recovered"
        padding_limit = max(128.0, metrics.get("frame_size_max", 0.0) * 4.0)
        if metrics.get("recovery_adaptive_padding_frames_delta_total", 0.0) > padding_limit:
            return "transient_stall_adaptive_padding_not_recovered"
        if metrics.get("adaptive_raise_events_total", 0.0) <= 0.0:
            return "transient_stall_adaptive_raise_not_observed"
        if metrics.get("adaptive_release_events_total", 0.0) <= 0.0 or \
                metrics.get("adaptive_target_recovered_frames_min", 0.0) <= 0.0:
            return "transient_stall_adaptive_target_not_recovering"
        return "pass"
    if scenario == "reorder-small":
        if proxy.get("client_to_server_reordered", 0) + proxy.get("server_to_client_reordered", 0) <= 0:
            return "proxy_reorder_not_injected"
        if metrics.get("sequence_out_of_order_total", 0.0) <= 0.0 and metrics.get("reordered_recovered_total", 0.0) <= 0.0:
            return "reorder_not_observed"
        return "pass"
    if scenario == "duplicate-2.0":
        injected = proxy.get("client_to_server_duplicated", 0) + proxy.get("server_to_client_duplicated", 0)
        if injected <= 0:
            return "proxy_duplicate_not_injected"
        if metrics.get("sequence_duplicate_total", 0.0) <= 0.0 and metrics.get("sequence_late_total", 0.0) <= 0.0:
            return "duplicate_not_observed"
        return "pass"
    if scenario == "corrupt-1.0":
        injected = proxy.get("client_to_server_corrupted", 0) + proxy.get("server_to_client_corrupted", 0)
        if injected <= 0:
            return "proxy_corruption_not_injected"
        if metrics.get("udp_parse_rejections_total", 0.0) <= 0.0:
            return "corruption_rejection_not_observed"
        return "pass"
    if scenario == "near-wrap-sequence":
        transformed = result.get("udp_validation", {}).get("transformer", {})
        if len(transformed.get("wrapped_directions", [])) != 2:
            return "sequence_wrap_not_injected_both_directions"
        if metrics.get("loss_percent_max", 0.0) > 0.0:
            return "false_loss_at_sequence_wrap"
        return "pass"
    if scenario == "malformed-udp":
        injections = result.get("udp_validation", {}).get("injections", [])
        successful = sum(1 for item in injections if item.get("injected", False))
        if successful < 16:
            return "malformed_corpus_not_fully_injected"
        if metrics.get("udp_parse_rejections_total", 0.0) < 16.0:
            return "malformed_rejections_not_observed"
        return "pass"
    if scenario == "delayed-replay":
        injections = result.get("udp_validation", {}).get("injections", [])
        if sum(1 for item in injections if item.get("injected", False)) != 2:
            return "replay_not_injected_both_directions"
        if metrics.get("sequence_duplicate_total", 0.0) <= 0.0 and metrics.get("sequence_late_total", 0.0) <= 0.0:
            return "replay_not_observed"
        return "pass"
    if scenario == "forward-sequence-gap":
        transformed = result.get("udp_validation", {}).get("transformer", {})
        if len(transformed.get("transformed_by_direction", {})) != 2:
            return "forward_gap_not_injected_both_directions"
        if metrics.get("udp_forward_gap_rejects_total", 0.0) < 2.0:
            return "forward_gap_rejections_not_observed"
        return "pass"
    if scenario == "extreme-sample-time":
        transformed = result.get("udp_validation", {}).get("transformer", {})
        if len(transformed.get("transformed_by_direction", {})) != 2:
            return "extreme_sample_time_not_injected_both_directions"
        if metrics.get("udp_sample_time_rejects_total", 0.0) < 2.0:
            return "sample_time_rejections_not_observed"
        return "pass"
    if scenario == "udp-short-flood":
        injections = result.get("udp_validation", {}).get("injections", [])
        if sum(item.get("injected", 0) for item in injections) < 4096:
            return "short_flood_not_fully_injected"
        if metrics.get("udp_parse_rejections_total", 0.0) <= 0.0:
            return "short_flood_rejections_not_observed"
        if metrics.get("udp_work_budget_yields_total", 0.0) <= 0.0:
            return "udp_work_budget_not_observed"
        return "pass"
    if scenario.startswith("jitter-buffer-"):
        if metrics.get("jitter_buffer_released_packets_total", 0.0) <= 0.0:
            return "jitter_buffer_not_used"
        if scenario.endswith("reorder-small"):
            if proxy.get("client_to_server_reordered", 0) + proxy.get("server_to_client_reordered", 0) <= 0:
                return "proxy_reorder_not_injected"
        return "pass"
    if scenario == "metronome-shared-grid":
        return metronome_verdict(result, "shared-grid")
    if is_listener_compensated_pulse_tracking_scenario(scenario):
        base = metronome_verdict(result, "listener-compensated")
        if base != "pass":
            return base
        if scenario == "metronome-listener-compensated-metro-pulse":
            metro_pulse = result.get("metro_pulse_epoch_analysis", {})
            if not metro_pulse:
                return "metro_pulse_epoch_analysis_missing"
            if not metro_pulse.get("ok", False):
                return metro_pulse.get("verdict", "metro_pulse_epoch_failed")
        pulse = result.get("listener_compensated_pulse_analysis", {})
        if not pulse:
            return "listener_compensated_pulse_analysis_missing"
        if not pulse.get("ok", False):
            return pulse.get("verdict", "listener_compensated_pulse_failed")
        if pulse.get("combined", {}).get("matched_pulses_min", 0) <= 0:
            return "listener_compensated_pulse_not_matched"
        if scenario == "metronome-listener-compensated-metro-pulse":
            if pulse.get("combined", {}).get("steady_samples_min", 0) <= 0:
                return "listener_compensated_pulse_steady_missing"
            max_abs_error = pulse.get("combined", {}).get("steady_max_abs_error_ms_max", 0.0)
            if max_abs_error > 80.0:
                return "listener_compensated_pulse_timing_high"
        return "pass"
    if scenario.startswith("metronome-"):
        expected = scenario.removeprefix("metronome-")
        return metronome_verdict(result, expected)
    if scenario.startswith("grid-authority-client-"):
        expected = scenario.removeprefix("grid-authority-client-")
        base = metronome_verdict(result, expected)
        if base != "pass":
            return base
        server = result.get("metrics", {}).get("server", {})
        client = result.get("metrics", {}).get("client", {})
        if (server.get("grid_authority_peer_id") != client.get("local_peer_id") or
                client.get("grid_authority_peer_id") != client.get("local_peer_id")):
            return "client_not_grid_authority"
        if min(server.get("grid_revision", 0), client.get("grid_revision", 0)) < 2:
            return "client_authority_revision_not_applied"
        if client.get("grid_proposals_sent", 0.0) <= 0.0:
            return "client_grid_proposal_not_sent"
        return "pass"
    if scenario == "grid-authority-concurrent":
        base = metronome_verdict(result, "shared-grid")
        if base != "pass":
            return base
        metrics_set = result.get("metrics", {})
        server = metrics_set.get("server", {})
        client = metrics_set.get("client", {})
        if min(server.get("grid_revision", 0), client.get("grid_revision", 0)) < 3:
            return "concurrent_grid_revisions_not_ordered"
        if server.get("grid_authority_peer_id") != client.get("grid_authority_peer_id"):
            return "concurrent_grid_authority_conflict"
        return "pass"
    if scenario == "transport-grid-authority":
        metric_set = result.get("metrics", {})
        server = metric_set.get("server", {})
        client = metric_set.get("client", {})
        if server.get("grid_authority_peer_id") != client.get("local_peer_id"):
            return "transport_grid_authority_not_client"
        if (server.get("arrangement_authority_peer_id") != server.get("local_peer_id") or
                client.get("arrangement_authority_peer_id") != server.get("local_peer_id")):
            return "transport_arrangement_authority_invalid"
        if (server.get("transport_source_peer_id") != server.get("local_peer_id") or
                client.get("transport_source_peer_id") != server.get("local_peer_id")):
            return "transport_source_identity_invalid"
        if min(server.get("transport_event_counter", 0), client.get("transport_event_counter", 0)) <= 0:
            return "transport_event_not_observed"
        if (server.get("transport_grid_revision") != server.get("grid_revision") or
                client.get("transport_grid_revision") != client.get("grid_revision")):
            return "transport_grid_revision_mismatch"
        if client.get("transport_events_accepted", 0.0) <= 0.0:
            return "transport_event_not_accepted"
        if client.get("transport_applied_target_frame", 0.0) <= client.get("transport_source_frame", 0.0):
            return "transport_applied_frame_invalid"
        return "pass"
    if scenario == "levels-low":
        server = result.get("metrics", {}).get("server", {})
        client = result.get("metrics", {}).get("client", {})
        if abs(server.get("final_metronome_level", 0.0) - 0.05) > 0.01:
            return "metronome_level_not_applied"
        if abs(client.get("final_remote_level", 0.0) - 0.50) > 0.01:
            return "remote_level_not_applied"
        return "pass"
    if scenario == "sample-time-playout-off":
        if result.get("metrics", {}).get("server", {}).get("sample_time_playout") != "off":
            return "sample_time_playout_not_off"
        return "pass"
    if scenario == "playout-delay-3072":
        if result.get("metrics", {}).get("server", {}).get("playout_delay_frames", 0.0) != 3072.0:
            return "playout_delay_not_applied"
        return "pass"
    if scenario == "drift-max-5ppm":
        ratio_min = metrics.get("resampler_ratio_min", 1.0)
        ratio_max = metrics.get("resampler_ratio_max", 1.0)
        if ratio_min < 0.999990 or ratio_max > 1.000010:
            return "resampler_exceeded_expected_cap"
        return "pass"
    if scenario == "socket-buffers":
        server = result.get("metrics", {}).get("server", {})
        if server.get("requested_socket_send_buffer_bytes", 0.0) <= 0.0:
            return "socket_buffers_not_requested"
        if server.get("socket_send_buffer_bytes", 0.0) <= 0.0 or server.get("socket_recv_buffer_bytes", 0.0) <= 0.0:
            return "socket_buffers_not_reported"
        return "pass"
    if scenario == "channels-1-to-1":
        server = result.get("metrics", {}).get("server", {})
        if "input=1" not in server.get("requested_channels", ""):
            return "input_channels_not_reported"
        if "output=1" not in server.get("requested_channels", ""):
            return "output_channels_not_reported"
        return "pass"
    if scenario == "runtime-controls":
        client = result.get("metrics", {}).get("client", {})
        observations = result.get("observations", {})
        if abs(client.get("final_metronome_level", 0.0) - 0.10) > 0.01:
            return "runtime_metronome_level_not_applied"
        if abs(client.get("final_remote_level", 0.0) - 0.75) > 0.01:
            return "runtime_remote_level_not_applied"
        if not observations.get("server_audio_control_metronome_mode_listener_compensated", False):
            return "runtime_metronome_mode_not_applied"
        return "pass"
    if scenario.startswith("audio-probe-"):
        audio = result.get("audio_probe_analysis", {})
        if not audio:
            return "audio_probe_missing"
        if not audio.get("ok", False):
            return audio.get("verdict", "audio_probe_failed")
        return "pass"
    return "pass"


def verdict_for(result):
    protocol_verdict = protocol_verdict_for(result)
    duration_verdict = duration_verdict_for(result)
    audio_health_failures = audio_health_failures_for(result)
    audio_health_observations = audio_health_observations_for(result)
    audio_health_verdict = audio_health_verdict_for(result)
    result["protocol_verdict"] = protocol_verdict
    result["duration_verdict"] = duration_verdict
    result["audio_health_verdict"] = audio_health_verdict
    result["audio_health_failures"] = audio_health_failures
    result["audio_health_observations"] = audio_health_observations
    result["duration_coverage_ratio_min"] = duration_coverage_ratio(result)
    if protocol_verdict != "pass":
        return protocol_verdict
    if duration_verdict != "pass":
        return duration_verdict
    if audio_health_verdict not in {"pass", "not_evaluated"}:
        return audio_health_verdict
    return "pass"


def metronome_verdict(result, expected_mode):
    metrics = result.get("metrics", {})
    combined = metrics.get("combined", {})
    server = metrics.get("server", {})
    client = metrics.get("client", {})
    wav = result.get("metronome_wav_analysis", {})
    if not combined.get("grid_authority_consensus", False):
        return "grid_authority_not_consistent"
    if not combined.get("grid_revision_consensus", False):
        return "grid_revision_not_consistent"
    if combined.get("grid_authority_states_sent_total", 0.0) <= 0.0:
        return "grid_authority_state_not_sent"
    if combined.get("grid_authority_states_accepted_total", 0.0) <= 0.0:
        return "grid_authority_state_not_accepted"
    if combined.get("metronome_alignment_valid_sides", 0.0) < 2 and not wav:
        return "metronome_alignment_not_valid"
    # Leader audio legitimately starts its creator clock at frame zero. The
    # alignment-valid fields above distinguish that from an unset epoch.
    if combined.get("grid_authority_epoch_min", 0.0) <= 0.0:
        return "metronome_epoch_not_set"
    if combined.get("grid_mapped_epoch_min", 0.0) <= 0.0:
        return "metronome_epoch_not_mapped"
    expected_mode_id = {
        "shared-grid": 0,
        "leader-audio": 1,
        "listener-compensated": 2,
    }.get(expected_mode, -1)
    if server.get("grid_mode", -1) != expected_mode_id or client.get("grid_mode", -1) != expected_mode_id:
        return "metronome_mode_not_applied"
    if expected_mode == "listener-compensated":
        if combined.get("metronome_compensation_active_sides", 0.0) < 1:
            return "metronome_compensation_not_active"
    elif expected_mode == "leader-audio":
        if combined.get("metronome_compensation_offset_ms_abs_max", 0.0) != 0.0:
            return "unexpected_metronome_compensation"
        if combined.get("leader_audio_injected_sides", 0) != 1:
            return "leader_audio_source_count_invalid"
        authority_id = combined.get("grid_authority_peer_id", 0)
        injecting = [side for side in (server, client) if side.get("leader_audio_injected_packets", 0.0) > 0.0]
        if len(injecting) != 1 or injecting[0].get("leader_audio_source_peer_id") != authority_id:
            return "leader_audio_source_not_authority"
    if wav:
        if not wav.get("ok", False):
            return wav.get("verdict", "metronome_wav_failed")
    return "pass"


def is_metronome_scenario(scenario):
    source = scenario.get("source_scenario") or scenario.get("scenario") or ""
    return source == "metronome-shared-grid" or source.startswith("metronome-")


def read_wav_i16(path):
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise ValueError("expected mono PCM16 WAV")
        sample_rate = handle.getframerate()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    samples = [
        int.from_bytes(raw[i:i + 2], "little", signed=True)
        for i in range(0, len(raw), 2)
    ]
    return sample_rate, samples


def detect_click_frames(samples):
    threshold = 1800
    holdoff = 900
    clicks = []
    hold = 0
    for index, sample in enumerate(samples):
        if hold > 0:
            hold -= 1
            continue
        if abs(sample) >= threshold:
            clicks.append(index)
            hold = holdoff
    return clicks


def expected_metronome_frames(meta, sample_count):
    sample_rate = int(meta.get("sample_rate", 48000))
    bpm = int(meta.get("bpm", 120))
    division = int(meta.get("metronome_division", 1))
    step_count = max(1, int(meta.get("metronome_step_count", 4)))
    play_low = int(meta.get("metronome_play_mask_low", 0x0f))
    play_high = int(meta.get("metronome_play_mask_high", 0))
    start_audio_frame = int(meta.get("start_audio_frame", 0))
    epoch = int(meta.get("metronome_epoch_sample_time", 0))
    epoch_valid = bool(meta.get("metronome_epoch_valid", False))
    if not epoch_valid or bpm <= 0 or division <= 0:
        return []
    step_interval = max(1, round((60.0 * sample_rate) / (bpm * division)))
    first_absolute = max(start_audio_frame, epoch)
    first_step = max(0, math.floor((first_absolute - epoch) / step_interval) - 1)
    expected = []
    step = first_step
    stop_audio_frame = start_audio_frame + sample_count
    while True:
        absolute = epoch + step * step_interval
        if absolute >= stop_audio_frame:
            break
        if absolute >= start_audio_frame:
            pattern_step = step % step_count
            mask = play_low if pattern_step < 64 else play_high
            bit = pattern_step if pattern_step < 64 else pattern_step - 64
            if ((mask >> bit) & 1) != 0:
                expected.append(absolute - start_audio_frame)
        step += 1
    return expected


def match_clicks(expected, detected):
    errors = []
    missing = 0
    extras = 0
    used = set()
    for frame in expected:
        best_index = None
        best_error = None
        for index, click in enumerate(detected):
            if index in used:
                continue
            error = abs(click - frame)
            if best_error is None or error < best_error:
                best_error = error
                best_index = index
        if best_index is None or best_error is None or best_error > METRONOME_WAV_TOLERANCE_FRAMES:
            missing += 1
        else:
            used.add(best_index)
            errors.append(best_error)
    extras = len(detected) - len(used)
    return {
        "expected_clicks": len(expected),
        "detected_clicks": len(detected),
        "missing_clicks": missing,
        "extra_clicks": extras,
        "max_abs_error_frames": max(errors, default=0),
        "avg_abs_error_frames": sum(errors) / len(errors) if errors else 0.0,
    }


def classify_metronome_clicks(expected, detected, analysis):
    startup_boundary = False
    steady_missing = analysis["missing_clicks"]
    steady_extra = analysis["extra_clicks"]
    steady_max_error = analysis["max_abs_error_frames"]
    if analysis["missing_clicks"] or analysis["extra_clicks"]:
        for detected_start in range(1, min(3, len(detected)) + 1):
            if len(expected) <= 1 or len(expected[1:]) != len(detected[detected_start:]):
                continue
            steady_errors = [abs(click - frame) for frame, click in zip(expected[1:], detected[detected_start:])]
            if not steady_errors or any(error > METRONOME_WAV_TOLERANCE_FRAMES for error in steady_errors):
                continue
            startup_boundary = True
            steady_missing = 0
            steady_extra = 0
            steady_max_error = max(steady_errors, default=0)
            break
    analysis.update({
        "startup_boundary_mismatch": startup_boundary,
        "steady_missing_clicks": steady_missing,
        "steady_extra_clicks": steady_extra,
        "steady_max_abs_error_frames": steady_max_error,
    })
    return analysis


def analyze_side_recording(recording_dir, allow_silent=False):
    meta_path = Path(recording_dir) / "recording.json"
    wav_path = Path(recording_dir) / "metronome.wav"
    if not meta_path.exists() or not wav_path.exists():
        return {"ok": False, "verdict": "metronome_wav_missing", "recording_dir": str(recording_dir)}
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    sample_rate, samples = read_wav_i16(wav_path)
    detected = detect_click_frames(samples)
    expected = expected_metronome_frames(meta, len(samples))
    if allow_silent and not detected:
        return {
            "ok": True,
            "recording_dir": str(recording_dir),
            "sample_rate": sample_rate,
            "expected_clicks": len(expected),
            "detected_clicks": 0,
            "silent_allowed": True,
        }
    analysis = classify_metronome_clicks(expected, detected, match_clicks(expected, detected))
    analysis.update({
        "ok": (
            analysis["steady_missing_clicks"] == 0 and
            analysis["steady_extra_clicks"] == 0 and
            analysis["steady_max_abs_error_frames"] <= METRONOME_WAV_TOLERANCE_FRAMES),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
    })
    if not analysis["ok"]:
        analysis["verdict"] = (
            "metronome_click_count_mismatch"
            if analysis["steady_missing_clicks"] > 0 or analysis["steady_extra_clicks"] > 0
            else "metronome_click_timing_high"
        )
    elif analysis["startup_boundary_mismatch"]:
        analysis["verdict"] = "pass_startup_boundary"
    return analysis


def analyze_metronome_recordings(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if not (scenario == "metronome-shared-grid" or scenario.startswith("metronome-")):
        return {}
    allow_client_silent = scenario == "metronome-leader-audio"
    loose_timing = scenario == "metronome-shared-grid" or scenario.startswith("metronome-listener-compensated")
    server = analyze_side_recording(Path(server_paths["dir"]) / "recording")
    client = analyze_side_recording(Path(client_paths["dir"]) / "recording", allow_silent=allow_client_silent)
    if loose_timing:
        server = analyze_metronome_wav(Path(server_paths["dir"]) / "recording")
        client = analyze_metronome_wav(Path(client_paths["dir"]) / "recording")
    ok = server.get("ok", False) and client.get("ok", False)
    verdict = ""
    if not ok:
        verdict = server.get("verdict") if not server.get("ok", False) else client.get("verdict", "metronome_wav_failed")
    return {"ok": ok, "verdict": verdict, "server": server, "client": client}


def is_listener_compensated_pulse_tracking_scenario(scenario):
    return (
        scenario == "metronome-listener-compensated-metro-pulse" or
        scenario.startswith("metronome-listener-compensated-pulse"))


def nearest_errors(reference_frames, measured_frames):
    errors = []
    used = set()
    missing = 0
    for reference in reference_frames:
        best_index = None
        best_abs = None
        best_signed = None
        for index, measured in enumerate(measured_frames):
            if index in used:
                continue
            signed = measured - reference
            absolute = abs(signed)
            if best_abs is None or absolute < best_abs:
                best_index = index
                best_abs = absolute
                best_signed = signed
        if best_index is None:
            missing += 1
            continue
        used.add(best_index)
        errors.append(best_signed)
    return errors, missing, max(0, len(measured_frames) - len(used))


def summarize_signed_errors(errors, sample_rate):
    if not errors:
        return {
            "samples": 0,
            "avg_error_frames": 0.0,
            "avg_error_ms": 0.0,
            "avg_abs_error_ms": 0.0,
            "max_abs_error_ms": 0.0,
            "min_error_ms": 0.0,
            "max_error_ms": 0.0,
        }
    ms = [error * 1000.0 / sample_rate for error in errors]
    abs_ms = [abs(value) for value in ms]
    return {
        "samples": len(errors),
        "avg_error_frames": sum(errors) / len(errors),
        "avg_error_ms": sum(ms) / len(ms),
        "avg_abs_error_ms": sum(abs_ms) / len(abs_ms),
        "max_abs_error_ms": max(abs_ms),
        "min_error_ms": min(ms),
        "max_error_ms": max(ms),
    }


def prefixed_summary(prefix, summary):
    return {f"{prefix}_{key}": value for key, value in summary.items()}


def read_stem_frames(recording_dir, stem, threshold, refractory_frames):
    wav_path = Path(recording_dir) / f"{stem}.wav"
    if not wav_path.exists():
        return 0, []
    sample_rate, samples = read_wav_i16(wav_path)
    return sample_rate, detect_click_frames_with_threshold(samples, threshold, refractory_frames)


def detect_click_frames_with_threshold(samples, threshold, refractory_frames):
    frames = []
    holdoff = 0
    for index, sample in enumerate(samples):
        if holdoff > 0:
            holdoff -= 1
            continue
        if abs(sample) >= threshold:
            frames.append(index)
            holdoff = refractory_frames
    return frames


def analyze_listener_compensated_pulse_side(recording_dir):
    recording_dir = Path(recording_dir)
    sample_rate, pulses = read_stem_frames(recording_dir, "their-input", 2500, 18000)
    metro_rate, clicks = read_stem_frames(recording_dir, "metronome", 1800, 900)
    if sample_rate == 0:
        sample_rate = metro_rate
    if sample_rate == 0:
        return {
            "ok": False,
            "verdict": "listener_compensated_pulse_recording_missing",
            "recording_dir": str(recording_dir),
            "sample_rate": 0,
            "remote_pulses_detected": len(pulses),
            "metronome_clicks_detected": len(clicks),
            "matched_pulses": 0,
            "missing_pulse_matches": 0,
            "extra_clicks": 0,
        }
    errors, missing_pulses, extra_clicks = nearest_errors(pulses, clicks)
    summary = summarize_signed_errors(errors, sample_rate)
    steady_errors = errors[LISTENER_PULSE_STEADY_SKIP:] if len(errors) > LISTENER_PULSE_STEADY_SKIP else []
    steady_summary = summarize_signed_errors(steady_errors, sample_rate)
    summary.update({
        "ok": bool(pulses) and bool(clicks),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
        "steady_skip_pulses": LISTENER_PULSE_STEADY_SKIP,
        "remote_pulses_detected": len(pulses),
        "metronome_clicks_detected": len(clicks),
        "matched_pulses": len(errors),
        "missing_pulse_matches": missing_pulses,
        "extra_clicks": extra_clicks,
        "verdict": "pass" if pulses and clicks else "pulse_or_click_missing",
    })
    summary.update(prefixed_summary("steady", steady_summary))
    return summary


def analyze_metro_pulse_epoch_side(recording_dir):
    recording_dir = Path(recording_dir)
    meta_path = recording_dir / "recording.json"
    wav_path = recording_dir / "my-input.wav"
    if not meta_path.exists() or not wav_path.exists():
        return {
            "ok": False,
            "verdict": "metro_pulse_epoch_recording_missing",
            "recording_dir": str(recording_dir),
        }
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    sample_rate, samples = read_wav_i16(wav_path)
    expected = expected_metronome_frames(meta, len(samples))
    detected = detect_click_frames_with_threshold(samples, 2500, 900)
    analysis = classify_metronome_clicks(expected, detected, match_clicks(expected, detected))
    analysis.update({
        "ok": (
            analysis["steady_missing_clicks"] == 0 and
            analysis["steady_extra_clicks"] == 0 and
            analysis["steady_max_abs_error_frames"] <= METRONOME_WAV_TOLERANCE_FRAMES),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
    })
    if not analysis["ok"]:
        analysis["verdict"] = (
            "metro_pulse_epoch_count_mismatch"
            if analysis["steady_missing_clicks"] > 0 or analysis["steady_extra_clicks"] > 0
            else "metro_pulse_epoch_timing_high"
        )
    elif analysis["startup_boundary_mismatch"]:
        analysis["verdict"] = "pass_startup_boundary"
    else:
        analysis["verdict"] = "pass"
    return analysis


def analyze_metro_pulse_epoch(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario != "metronome-listener-compensated-metro-pulse":
        return {}
    server = analyze_metro_pulse_epoch_side(Path(server_paths["dir"]) / "recording")
    client = analyze_metro_pulse_epoch_side(Path(client_paths["dir"]) / "recording")
    ok = server.get("ok", False) and client.get("ok", False)
    return {
        "ok": ok,
        "verdict": "pass" if ok else "metro_pulse_epoch_analysis_failed",
        "server": server,
        "client": client,
        "combined": {
            "max_abs_error_frames_max": max(
                server.get("steady_max_abs_error_frames", 0),
                client.get("steady_max_abs_error_frames", 0)),
            "missing_clicks_total": server.get("steady_missing_clicks", 0) + client.get("steady_missing_clicks", 0),
            "extra_clicks_total": server.get("steady_extra_clicks", 0) + client.get("steady_extra_clicks", 0),
        },
    }


def analyze_listener_compensated_pulse(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if not is_listener_compensated_pulse_tracking_scenario(scenario):
        return {}
    server = analyze_listener_compensated_pulse_side(Path(server_paths["dir"]) / "recording")
    client = analyze_listener_compensated_pulse_side(Path(client_paths["dir"]) / "recording")
    ok = server.get("ok", False) and client.get("ok", False)
    return {
        "ok": ok,
        "verdict": "pass" if ok else "listener_compensated_pulse_analysis_failed",
        "server": server,
        "client": client,
        "combined": {
            "matched_pulses_min": min(server.get("matched_pulses", 0), client.get("matched_pulses", 0)),
            "avg_abs_error_ms_max": max(server.get("avg_abs_error_ms", 0.0), client.get("avg_abs_error_ms", 0.0)),
            "max_abs_error_ms_max": max(server.get("max_abs_error_ms", 0.0), client.get("max_abs_error_ms", 0.0)),
            "steady_samples_min": min(server.get("steady_samples", 0), client.get("steady_samples", 0)),
            "steady_avg_abs_error_ms_max": max(
                server.get("steady_avg_abs_error_ms", 0.0),
                client.get("steady_avg_abs_error_ms", 0.0)),
            "steady_max_abs_error_ms_max": max(
                server.get("steady_max_abs_error_ms", 0.0),
                client.get("steady_max_abs_error_ms", 0.0)),
            "missing_pulse_matches_total": server.get("missing_pulse_matches", 0) + client.get("missing_pulse_matches", 0),
        },
    }


def run_runtime_commands(commands, server_process, client_process):
    start = time.monotonic()
    for command in sorted(commands, key=lambda item: item.get("at_s", 0.0)):
        delay = start + float(command.get("at_s", 0.0)) - time.monotonic()
        if delay > 0.0:
            time.sleep(delay)
        target = server_process if command.get("side") == "server" else client_process
        target.send_line(command.get("line", ""))


def text_contains(path, pattern):
    try:
        return pattern in Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def scenario_observations(scenario_id, scenario, server_paths, client_paths):
    source = scenario.get("source_scenario", scenario_id)
    observations = {}
    if source == "runtime-controls":
        observations["server_audio_control_metronome_mode_listener_compensated"] = text_contains(
            server_paths["stdout"],
            "Audio control metronome mode: listener-compensated")
        observations["client_final_metronome_level_0_1"] = text_contains(
            client_paths["stdout"],
            "Final metronome level: 0.1")
        observations["client_final_remote_level_0_75"] = text_contains(
            client_paths["stdout"],
            "Final remote playback level: 0.75")
    return observations
