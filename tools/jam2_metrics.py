#!/usr/bin/env python3

import csv
from pathlib import Path


FINAL_ROW = "final"
PERIODIC_ROW = "periodic"


def to_float(row, field):
    value = row.get(field, "")
    if value == "":
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def read_rows(path):
    path = Path(path)
    if not path.exists():
        return []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def sample_rate_for_row(row):
    return (
        to_float(row, "active_sample_rate")
        or to_float(row, "requested_sample_rate")
        or to_float(row, "sample_rate")
        or 48000.0
    )


def final_row(rows):
    finals = [row for row in rows if row.get("row_type") == FINAL_ROW]
    if finals:
        return finals[-1]
    return rows[-1] if rows else {}


def periodic_rows(rows):
    return [row for row in rows if row.get("row_type") == PERIODIC_ROW]


def summarize_csv(path):
    rows = read_rows(path)
    row = final_row(rows)
    periods = periodic_rows(rows)
    if not row:
        return {"csv_path": str(path), "has_csv": False}

    elapsed_s = to_float(row, "elapsed_ms") / 1000.0
    recv_packets = to_float(row, "recv_packets")
    sequence_lost = to_float(row, "sequence_lost")
    reordered_lost = to_float(row, "reordered_lost")
    loss = max(sequence_lost, reordered_lost)
    loss_percent = loss * 100.0 / max(1.0, recv_packets + sequence_lost)
    underrun_time_ms = to_float(row, "playback_ring_underrun_time_ms")
    if underrun_time_ms <= 0.0:
        underrun_time_ms = to_float(row, "playback_ring_underruns") * 1000.0 / sample_rate_for_row(row)

    periodic_depths = [to_float(period, "playback_depth_avg_ms") for period in periods]
    return {
        "csv_path": str(path),
        "has_csv": True,
        "requested_sample_rate": to_float(row, "requested_sample_rate") or to_float(row, "sample_rate"),
        "active_sample_rate": to_float(row, "active_sample_rate") or sample_rate_for_row(row),
        "frame_size": to_float(row, "frame_size"),
        "requested_audio_buffer_frames": to_float(row, "requested_audio_buffer_frames"),
        "active_audio_buffer_frames": to_float(row, "active_audio_buffer_frames"),
        "elapsed_s": elapsed_s,
        "sent_packets": to_float(row, "sent_packets"),
        "recv_packets": recv_packets,
        "sequence_lost": sequence_lost,
        "sequence_loss_percent": loss_percent,
        "sequence_out_of_order": to_float(row, "sequence_out_of_order"),
        "sequence_duplicate": to_float(row, "sequence_duplicate"),
        "sequence_late": to_float(row, "sequence_late"),
        "reordered_recovered": to_float(row, "reordered_recovered"),
        "reordered_lost": reordered_lost,
        "jitter_avg_ms": to_float(row, "jitter_avg_ms"),
        "jitter_max_ms": to_float(row, "jitter_max_ms"),
        "rtt_avg_ms": to_float(row, "rtt_avg_ms"),
        "rtt_max_ms": to_float(row, "rtt_max_ms"),
        "estimated_one_way_ms": to_float(row, "estimated_one_way_ms"),
        "playback_depth_min_ms": to_float(row, "playback_depth_min_ms"),
        "playback_depth_avg_ms": to_float(row, "playback_depth_avg_ms"),
        "playback_depth_max_ms": to_float(row, "playback_depth_max_ms"),
        "periodic_playback_depth_avg_ms_max": max(periodic_depths, default=0.0),
        "playback_ring_underruns": to_float(row, "playback_ring_underruns"),
        "playback_ring_underrun_events": to_float(row, "playback_ring_underrun_events"),
        "playback_ring_underrun_time_ms": underrun_time_ms,
        "playback_ring_underrun_time_ms_per_min": underrun_time_ms * 60.0 / elapsed_s if elapsed_s > 0.0 else 0.0,
        "playback_ring_underrun_burst_max_ms": to_float(row, "playback_ring_underrun_burst_max_ms"),
        "playback_ring_overruns": to_float(row, "playback_ring_overruns"),
        "playback_dropped_frames": to_float(row, "playback_dropped_frames"),
        "playback_dropped_time_ms": to_float(row, "playback_dropped_time_ms"),
        "missing_audio_frames_inserted": to_float(row, "missing_audio_frames_inserted"),
        "late_audio_frames_dropped": to_float(row, "late_audio_frames_dropped"),
        "drift_ppm": to_float(row, "drift_ppm"),
        "raw_drift_ppm": to_float(row, "raw_drift_ppm"),
        "resampler_ratio": to_float(row, "resampler_ratio"),
        "resampler_ratio_min": to_float(row, "resampler_ratio_min"),
        "resampler_ratio_avg": to_float(row, "resampler_ratio_avg"),
        "resampler_ratio_max": to_float(row, "resampler_ratio_max"),
        "drift_correction_active_percent": to_float(row, "drift_correction_active_percent"),
        "drift_correction_clamped_percent": to_float(row, "drift_correction_clamped_percent"),
        "resampler_ratio_change_max_ppm_per_second": to_float(row, "resampler_ratio_change_max_ppm_per_second"),
        "adaptive_playback_raise_events": to_float(row, "adaptive_playback_raise_events"),
        "adaptive_playback_release_events": to_float(row, "adaptive_playback_release_events"),
        "adaptive_playback_burst_events": to_float(row, "adaptive_playback_burst_events"),
        "audio_callbacks": to_float(row, "audio_callbacks"),
        "audio_callback_interval_min_ms": to_float(row, "audio_callback_interval_min_ms"),
        "audio_callback_interval_avg_ms": to_float(row, "audio_callback_interval_avg_ms"),
        "audio_callback_interval_max_ms": to_float(row, "audio_callback_interval_max_ms"),
        "audio_callback_interval_samples": to_float(row, "audio_callback_interval_samples"),
        "audio_callback_gap_over_1_1x_count": to_float(row, "audio_callback_gap_over_1_1x_count"),
        "audio_callback_gap_over_1_5x_count": to_float(row, "audio_callback_gap_over_1_5x_count"),
        "audio_callback_gap_over_2x_count": to_float(row, "audio_callback_gap_over_2x_count"),
        "metronome_sent": to_float(row, "metronome_states_sent") or to_float(row, "metronome_sent"),
        "metronome_received": to_float(row, "metronome_states_received") or to_float(row, "metronome_received"),
        "metronome_alignment_valid": row.get("metronome_alignment_valid", ""),
        "metronome_epoch_sample_time": to_float(row, "metronome_epoch_sample_time"),
        "local_metronome_beat": to_float(row, "local_metronome_beat"),
        "remote_metronome_beat": to_float(row, "remote_metronome_beat"),
        "metronome_beat_delta_abs": abs(to_float(row, "local_metronome_beat") - to_float(row, "remote_metronome_beat")),
        "final_metronome": row.get("final_metronome", ""),
        "final_bpm": to_float(row, "final_bpm"),
        "metronome_level": to_float(row, "metronome_level"),
        "remote_level": to_float(row, "remote_level"),
        "final_metronome_level": to_float(row, "final_metronome_level"),
        "final_remote_level": to_float(row, "final_remote_level"),
        "metronome_mode": row.get("metronome_mode", ""),
        "metronome_compensation_active": row.get("metronome_compensation_active", ""),
        "metronome_compensation_offset_frames": to_float(row, "metronome_compensation_offset_frames"),
        "metronome_compensation_offset_ms": to_float(row, "metronome_compensation_offset_ms"),
        "metronome_compensation_target_frames": to_float(row, "metronome_compensation_target_frames"),
        "metronome_compensation_target_ms": to_float(row, "metronome_compensation_target_ms"),
        "metronome_compensation_clamp_events": to_float(row, "metronome_compensation_clamp_events"),
        "metronome_compensation_stale_events": to_float(row, "metronome_compensation_stale_events"),
        "sample_time_playout": row.get("sample_time_playout", ""),
        "playout_delay_frames": to_float(row, "playout_delay_frames"),
        "playout_delay_ms": to_float(row, "playout_delay_ms"),
        "playout_delay_error_ms": to_float(row, "playout_delay_error_ms"),
        "jitter_buffer": row.get("jitter_buffer", ""),
        "jitter_buffer_target_frames": to_float(row, "jitter_buffer_target_frames"),
        "jitter_buffer_target_ms": to_float(row, "jitter_buffer_target_ms"),
        "jitter_buffer_max_frames": to_float(row, "jitter_buffer_max_frames"),
        "jitter_buffer_depth_max_frames": to_float(row, "jitter_buffer_depth_max_frames"),
        "jitter_buffer_queued_packets": to_float(row, "jitter_buffer_queued_packets"),
        "jitter_buffer_released_packets": to_float(row, "jitter_buffer_released_packets"),
        "jitter_buffer_late_packets": to_float(row, "jitter_buffer_late_packets"),
        "jitter_buffer_dropped_packets": to_float(row, "jitter_buffer_dropped_packets"),
        "jitter_buffer_dropped_frames": to_float(row, "jitter_buffer_dropped_frames"),
        "jitter_buffer_forced_releases": to_float(row, "jitter_buffer_forced_releases"),
        "socket_send_buffer_bytes": to_float(row, "socket_send_buffer_bytes"),
        "socket_recv_buffer_bytes": to_float(row, "socket_recv_buffer_bytes"),
        "requested_socket_send_buffer_bytes": to_float(row, "requested_socket_send_buffer_bytes"),
        "requested_socket_recv_buffer_bytes": to_float(row, "requested_socket_recv_buffer_bytes"),
        "requested_channels": row.get("requested_channels", ""),
        "active_channels": row.get("active_channels", ""),
        "udp_short_packets": to_float(row, "udp_short_packets"),
        "udp_wrong_magic": to_float(row, "udp_wrong_magic"),
        "udp_wrong_version": to_float(row, "udp_wrong_version"),
        "udp_unknown_type": to_float(row, "udp_unknown_type"),
        "udp_invalid_flags": to_float(row, "udp_invalid_flags"),
        "udp_invalid_reserved": to_float(row, "udp_invalid_reserved"),
        "udp_wrong_session": to_float(row, "udp_wrong_session"),
        "udp_invalid_payload_size": to_float(row, "udp_invalid_payload_size"),
        "udp_authentication_failed": to_float(row, "udp_authentication_failed"),
        "udp_replay_rejects": to_float(row, "udp_replay_rejects"),
        "udp_forward_gap_rejects": to_float(row, "udp_forward_gap_rejects"),
        "udp_forward_gap_resyncs": to_float(row, "udp_forward_gap_resyncs"),
        "udp_sequence_ambiguous_rejects": to_float(row, "udp_sequence_ambiguous_rejects"),
        "udp_sample_time_stale_rejects": to_float(row, "udp_sample_time_stale_rejects"),
        "udp_sample_time_future_rejects": to_float(row, "udp_sample_time_future_rejects"),
        "udp_unmatched_pongs": to_float(row, "udp_unmatched_pongs"),
        "udp_ping_slot_overwrites": to_float(row, "udp_ping_slot_overwrites"),
        "udp_work_budget_yields": to_float(row, "udp_work_budget_yields"),
        "reorder_pending_high_water": to_float(row, "reorder_pending_high_water"),
        "reorder_capacity_drops": to_float(row, "reorder_capacity_drops"),
        "jitter_pending_high_water": to_float(row, "jitter_pending_high_water"),
        "jitter_capacity_drops": to_float(row, "jitter_capacity_drops"),
    }


def combined_summary(server_csv, client_csv):
    server = summarize_csv(server_csv) if server_csv else {"has_csv": False}
    client = summarize_csv(client_csv) if client_csv else {"has_csv": False}
    sides = [side for side in (server, client) if side.get("has_csv")]
    if not sides:
        return {
            "server": server,
            "client": client,
            "combined": {"has_csv": False},
        }
    combined = {
        "has_csv": True,
        "server_active_sample_rate": server.get("active_sample_rate", 0.0),
        "client_active_sample_rate": client.get("active_sample_rate", 0.0),
        "server_requested_sample_rate": server.get("requested_sample_rate", 0.0),
        "client_requested_sample_rate": client.get("requested_sample_rate", 0.0),
        "elapsed_s": max((side.get("elapsed_s", 0.0) for side in sides), default=0.0),
        "elapsed_s_min": min((side.get("elapsed_s", 0.0) for side in sides), default=0.0),
        "frame_size_max": max((side.get("frame_size", 0.0) for side in sides), default=0.0),
        "audio_callbacks_min": min((side.get("audio_callbacks", 0.0) for side in sides), default=0.0),
        "audio_callback_interval_avg_ms_max": max(
            (side.get("audio_callback_interval_avg_ms", 0.0) for side in sides), default=0.0),
        "audio_callback_interval_max_ms_max": max(
            (side.get("audio_callback_interval_max_ms", 0.0) for side in sides), default=0.0),
        "audio_callback_gap_over_2x_total": sum(
            (side.get("audio_callback_gap_over_2x_count", 0.0) for side in sides), 0.0),
        "loss_percent_max": max((side.get("sequence_loss_percent", 0.0) for side in sides), default=0.0),
        "jitter_max_ms": max((side.get("jitter_max_ms", 0.0) for side in sides), default=0.0),
        "rtt_max_ms": max((side.get("rtt_max_ms", 0.0) for side in sides), default=0.0),
        "playback_underrun_time_ms_total": sum((side.get("playback_ring_underrun_time_ms", 0.0) for side in sides), 0.0),
        "playback_underrun_burst_max_ms": max((side.get("playback_ring_underrun_burst_max_ms", 0.0) for side in sides), default=0.0),
        "playback_overruns_total": sum((side.get("playback_ring_overruns", 0.0) for side in sides), 0.0),
        "playback_dropped_frames_total": sum((side.get("playback_dropped_frames", 0.0) for side in sides), 0.0),
        "missing_audio_frames_total": sum((side.get("missing_audio_frames_inserted", 0.0) for side in sides), 0.0),
        "late_audio_frames_total": sum((side.get("late_audio_frames_dropped", 0.0) for side in sides), 0.0),
        "drift_abs_ppm_max": max((abs(side.get("drift_ppm", 0.0)) for side in sides), default=0.0),
        "adaptive_raise_events_total": sum((side.get("adaptive_playback_raise_events", 0.0) for side in sides), 0.0),
        "adaptive_burst_events_total": sum((side.get("adaptive_playback_burst_events", 0.0) for side in sides), 0.0),
        "metronome_received_min": min((side.get("metronome_received", 0.0) for side in sides), default=0.0),
        "metronome_epoch_sample_time_min": min((side.get("metronome_epoch_sample_time", 0.0) for side in sides), default=0.0),
        "local_metronome_beat_max": max((side.get("local_metronome_beat", 0.0) for side in sides), default=0.0),
        "remote_metronome_beat_max": max((side.get("remote_metronome_beat", 0.0) for side in sides), default=0.0),
        "metronome_beat_delta_abs_max": max((side.get("metronome_beat_delta_abs", 0.0) for side in sides), default=0.0),
        "metronome_alignment_valid_sides": sum(
            (1 for side in sides if side.get("metronome_alignment_valid", "") == "yes"), 0),
        "metronome_compensation_active_sides": sum(
            (1 for side in sides if side.get("metronome_compensation_active", "") == "yes"), 0),
        "metronome_compensation_offset_ms_abs_max": max(
            (abs(side.get("metronome_compensation_offset_ms", 0.0)) for side in sides), default=0.0),
        "metronome_compensation_target_ms_abs_max": max(
            (abs(side.get("metronome_compensation_target_ms", 0.0)) for side in sides), default=0.0),
        "metronome_compensation_clamp_events_total": sum(
            (side.get("metronome_compensation_clamp_events", 0.0) for side in sides), 0.0),
        "metronome_compensation_stale_events_total": sum(
            (side.get("metronome_compensation_stale_events", 0.0) for side in sides), 0.0),
        "sequence_lost_total": sum((side.get("sequence_lost", 0.0) for side in sides), 0.0),
        "sequence_out_of_order_total": sum((side.get("sequence_out_of_order", 0.0) for side in sides), 0.0),
        "sequence_duplicate_total": sum((side.get("sequence_duplicate", 0.0) for side in sides), 0.0),
        "sequence_late_total": sum((side.get("sequence_late", 0.0) for side in sides), 0.0),
        "reordered_recovered_total": sum((side.get("reordered_recovered", 0.0) for side in sides), 0.0),
        "reordered_lost_total": sum((side.get("reordered_lost", 0.0) for side in sides), 0.0),
        "drift_correction_clamped_percent_max": max((side.get("drift_correction_clamped_percent", 0.0) for side in sides), default=0.0),
        "resampler_ratio_min": min((side.get("resampler_ratio_min", 1.0) for side in sides), default=1.0),
        "resampler_ratio_max": max((side.get("resampler_ratio_max", 1.0) for side in sides), default=1.0),
        "playout_delay_error_ms_abs_max": max((abs(side.get("playout_delay_error_ms", 0.0)) for side in sides), default=0.0),
        "jitter_buffer_late_packets_total": sum((side.get("jitter_buffer_late_packets", 0.0) for side in sides), 0.0),
        "jitter_buffer_dropped_packets_total": sum((side.get("jitter_buffer_dropped_packets", 0.0) for side in sides), 0.0),
        "jitter_buffer_dropped_frames_total": sum((side.get("jitter_buffer_dropped_frames", 0.0) for side in sides), 0.0),
        "jitter_buffer_forced_releases_total": sum(
            (side.get("jitter_buffer_forced_releases", 0.0) for side in sides), 0.0),
        "jitter_buffer_released_packets_total": sum((side.get("jitter_buffer_released_packets", 0.0) for side in sides), 0.0),
        "jitter_buffer_depth_max_frames": max((side.get("jitter_buffer_depth_max_frames", 0.0) for side in sides), default=0.0),
        "udp_parse_rejections_total": sum((
            side.get("udp_short_packets", 0.0) +
            side.get("udp_wrong_magic", 0.0) +
            side.get("udp_wrong_version", 0.0) +
            side.get("udp_unknown_type", 0.0) +
            side.get("udp_invalid_flags", 0.0) +
            side.get("udp_invalid_reserved", 0.0) +
            side.get("udp_wrong_session", 0.0) +
            side.get("udp_invalid_payload_size", 0.0) +
            side.get("udp_authentication_failed", 0.0)
            for side in sides), 0.0),
        "udp_replay_rejects_total": sum((side.get("udp_replay_rejects", 0.0) for side in sides), 0.0),
        "udp_forward_gap_rejects_total": sum((side.get("udp_forward_gap_rejects", 0.0) for side in sides), 0.0),
        "udp_forward_gap_resyncs_total": sum((side.get("udp_forward_gap_resyncs", 0.0) for side in sides), 0.0),
        "udp_sample_time_stale_rejects_total": sum(
            (side.get("udp_sample_time_stale_rejects", 0.0) for side in sides), 0.0),
        "udp_sample_time_future_rejects_total": sum(
            (side.get("udp_sample_time_future_rejects", 0.0) for side in sides), 0.0),
        "udp_sample_time_rejects_total": sum((
            side.get("udp_sample_time_stale_rejects", 0.0) +
            side.get("udp_sample_time_future_rejects", 0.0)
            for side in sides), 0.0),
        "udp_unmatched_pongs_total": sum((side.get("udp_unmatched_pongs", 0.0) for side in sides), 0.0),
        "udp_work_budget_yields_total": sum((side.get("udp_work_budget_yields", 0.0) for side in sides), 0.0),
        "reorder_pending_high_water_max": max((side.get("reorder_pending_high_water", 0.0) for side in sides), default=0.0),
        "reorder_capacity_drops_total": sum((side.get("reorder_capacity_drops", 0.0) for side in sides), 0.0),
        "jitter_pending_high_water_max": max((side.get("jitter_pending_high_water", 0.0) for side in sides), default=0.0),
        "jitter_capacity_drops_total": sum((side.get("jitter_capacity_drops", 0.0) for side in sides), 0.0),
    }
    return {"server": server, "client": client, "combined": combined}


def write_results_csv(path, results):
    fields = [
        "scenario",
        "verdict",
        "protocol_verdict",
        "duration_verdict",
        "audio_health_verdict",
        "audio_health_failures",
        "audio_health_observations",
        "duration_coverage_ratio_min",
        "source_scenario",
        "profile_family",
        "profile",
        "os_priority",
        "return_code_server",
        "return_code_client",
        "proxy_client_to_server_packets",
        "proxy_server_to_client_packets",
        "proxy_client_to_server_dropped",
        "proxy_server_to_client_dropped",
        "proxy_client_to_server_blackout_events",
        "proxy_server_to_client_blackout_events",
        "proxy_client_to_server_recv_errors",
        "proxy_server_to_client_recv_errors",
        "proxy_client_to_server_send_errors",
        "proxy_server_to_client_send_errors",
        "loss_percent_max",
        "elapsed_s_min",
        "audio_callbacks_min",
        "audio_callback_interval_avg_ms_max",
        "audio_callback_interval_max_ms_max",
        "audio_callback_gap_over_2x_total",
        "jitter_max_ms",
        "rtt_max_ms",
        "playback_underrun_time_ms_total",
        "playback_underrun_burst_max_ms",
        "playback_overruns_total",
        "playback_dropped_frames_total",
        "missing_audio_frames_total",
        "late_audio_frames_total",
        "drift_abs_ppm_max",
        "adaptive_raise_events_total",
        "adaptive_burst_events_total",
        "metronome_received_min",
        "metronome_epoch_sample_time_min",
        "local_metronome_beat_max",
        "remote_metronome_beat_max",
        "metronome_beat_delta_abs_max",
        "metronome_alignment_valid_sides",
        "metronome_compensation_active_sides",
        "metronome_compensation_offset_ms_abs_max",
        "metronome_compensation_target_ms_abs_max",
        "metronome_compensation_clamp_events_total",
        "metronome_compensation_stale_events_total",
        "sequence_lost_total",
        "sequence_out_of_order_total",
        "sequence_duplicate_total",
        "sequence_late_total",
        "reordered_recovered_total",
        "reordered_lost_total",
        "drift_correction_clamped_percent_max",
        "resampler_ratio_min",
        "resampler_ratio_max",
        "playout_delay_error_ms_abs_max",
        "jitter_buffer_late_packets_total",
        "jitter_buffer_dropped_packets_total",
        "jitter_buffer_dropped_frames_total",
        "jitter_buffer_forced_releases_total",
        "jitter_buffer_released_packets_total",
        "jitter_buffer_depth_max_frames",
        "udp_parse_rejections_total",
        "udp_replay_rejects_total",
        "udp_forward_gap_rejects_total",
        "udp_sample_time_stale_rejects_total",
        "udp_sample_time_future_rejects_total",
        "udp_work_budget_yields_total",
        "reorder_capacity_drops_total",
        "jitter_capacity_drops_total",
        "metronome_wav_ok",
        "metronome_wav_verdict",
        "audio_probe_ok",
        "audio_probe_verdict",
        "audio_probe_signal",
        "audio_probe_server_signal",
        "audio_probe_client_signal",
        "audio_probe_tags",
        "listener_pulse_ok",
        "listener_pulse_verdict",
        "listener_pulse_matched_min",
        "listener_pulse_avg_abs_error_ms_max",
        "listener_pulse_max_abs_error_ms_max",
        "listener_pulse_steady_samples_min",
        "listener_pulse_steady_avg_abs_error_ms_max",
        "listener_pulse_steady_max_abs_error_ms_max",
        "listener_pulse_missing_matches_total",
        "metro_pulse_epoch_ok",
        "metro_pulse_epoch_verdict",
        "metro_pulse_epoch_max_error_frames",
        "metro_pulse_epoch_missing_total",
        "metro_pulse_epoch_extra_total",
        "mesh_peers",
        "mesh_peers_with_csv",
        "mesh_audio_ok_peers",
        "mesh_recv_packets_min",
        "mesh_sent_packets_min",
        "mesh_sequence_lost_total",
        "mesh_stats_duration_coverage_ratio_min",
        "mesh_recording_duration_coverage_ratio_min",
        "mesh_audio_callbacks_min",
        "mesh_audio_callback_interval_avg_ms_max",
        "mesh_audio_callback_gap_over_2x_total",
        "mesh_udp_sample_time_stale_rejects_total",
        "mesh_udp_sample_time_future_rejects_total",
        "mesh_missing_audio_frames_total",
        "mesh_playback_dropped_frames_total",
        "mesh_jitter_buffer_forced_releases_total",
        "mesh_audio_tags",
        "server_metronome_wav_max_error_frames",
        "client_metronome_wav_max_error_frames",
        "server_metronome_wav_missing_clicks",
        "client_metronome_wav_missing_clicks",
        "server_csv_path",
        "client_csv_path",
    ]
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for result in results:
            combined = result.get("metrics", {}).get("combined", {})
            proxy = result.get("proxy_stats", {})
            wav = result.get("metronome_wav_analysis", {})
            audio_probe = result.get("audio_probe_analysis", {})
            listener_pulse = result.get("listener_compensated_pulse_analysis", {})
            listener_pulse_combined = listener_pulse.get("combined", {})
            metro_pulse = result.get("metro_pulse_epoch_analysis", {})
            metro_pulse_combined = metro_pulse.get("combined", {})
            mesh = result.get("mesh_metrics", {})
            wav_server = wav.get("server", {})
            wav_client = wav.get("client", {})
            row = {
                "scenario": result.get("scenario", ""),
                "verdict": result.get("verdict", ""),
                "protocol_verdict": result.get("protocol_verdict", ""),
                "duration_verdict": result.get("duration_verdict", ""),
                "audio_health_verdict": result.get("audio_health_verdict", ""),
                "audio_health_failures": ";".join(result.get("audio_health_failures", [])),
                "audio_health_observations": ";".join(result.get("audio_health_observations", [])),
                "duration_coverage_ratio_min": result.get("duration_coverage_ratio_min", ""),
                "source_scenario": result.get("source_scenario", result.get("scenario", "")),
                "profile_family": result.get("profile_family", ""),
                "profile": result.get("profile", ""),
                "os_priority": result.get("os_priority", ""),
                "return_code_server": result.get("server_return_code", ""),
                "return_code_client": result.get("client_return_code", ""),
                "proxy_client_to_server_packets": proxy.get("client_to_server_packets", 0),
                "proxy_server_to_client_packets": proxy.get("server_to_client_packets", 0),
                "proxy_client_to_server_dropped": proxy.get("client_to_server_dropped", 0),
                "proxy_server_to_client_dropped": proxy.get("server_to_client_dropped", 0),
                "proxy_client_to_server_blackout_events": proxy.get("client_to_server_blackout_events", 0),
                "proxy_server_to_client_blackout_events": proxy.get("server_to_client_blackout_events", 0),
                "proxy_client_to_server_recv_errors": proxy.get("client_to_server_recv_errors", 0),
                "proxy_server_to_client_recv_errors": proxy.get("server_to_client_recv_errors", 0),
                "proxy_client_to_server_send_errors": proxy.get("client_to_server_send_errors", 0),
                "proxy_server_to_client_send_errors": proxy.get("server_to_client_send_errors", 0),
                "metronome_wav_ok": "yes" if wav.get("ok", False) else ("no" if wav else ""),
                "metronome_wav_verdict": wav.get("verdict", ""),
                "audio_probe_ok": "yes" if audio_probe.get("ok", False) else ("no" if audio_probe else ""),
                "audio_probe_verdict": audio_probe.get("verdict", ""),
                "audio_probe_signal": audio_probe.get("signal", ""),
                "audio_probe_server_signal": audio_probe.get("server_signal", ""),
                "audio_probe_client_signal": audio_probe.get("client_signal", ""),
                "audio_probe_tags": ";".join(audio_probe.get("tags", [])),
                "listener_pulse_ok": "yes" if listener_pulse.get("ok", False) else ("no" if listener_pulse else ""),
                "listener_pulse_verdict": listener_pulse.get("verdict", ""),
                "listener_pulse_matched_min": listener_pulse_combined.get("matched_pulses_min", ""),
                "listener_pulse_avg_abs_error_ms_max": listener_pulse_combined.get("avg_abs_error_ms_max", ""),
                "listener_pulse_max_abs_error_ms_max": listener_pulse_combined.get("max_abs_error_ms_max", ""),
                "listener_pulse_steady_samples_min": listener_pulse_combined.get("steady_samples_min", ""),
                "listener_pulse_steady_avg_abs_error_ms_max": listener_pulse_combined.get("steady_avg_abs_error_ms_max", ""),
                "listener_pulse_steady_max_abs_error_ms_max": listener_pulse_combined.get("steady_max_abs_error_ms_max", ""),
                "listener_pulse_missing_matches_total": listener_pulse_combined.get("missing_pulse_matches_total", ""),
                "metro_pulse_epoch_ok": "yes" if metro_pulse.get("ok", False) else ("no" if metro_pulse else ""),
                "metro_pulse_epoch_verdict": metro_pulse.get("verdict", ""),
                "metro_pulse_epoch_max_error_frames": metro_pulse_combined.get("max_abs_error_frames_max", ""),
                "metro_pulse_epoch_missing_total": metro_pulse_combined.get("missing_clicks_total", ""),
                "metro_pulse_epoch_extra_total": metro_pulse_combined.get("extra_clicks_total", ""),
                "mesh_peers": mesh.get("peer_count", ""),
                "mesh_peers_with_csv": mesh.get("peers_with_csv", ""),
                "mesh_audio_ok_peers": mesh.get("audio_ok_peers", ""),
                "mesh_recv_packets_min": mesh.get("recv_packets_min", ""),
                "mesh_sent_packets_min": mesh.get("sent_packets_min", ""),
                "mesh_sequence_lost_total": mesh.get("sequence_lost_total", ""),
                "mesh_stats_duration_coverage_ratio_min": mesh.get("stats_duration_coverage_ratio_min", ""),
                "mesh_recording_duration_coverage_ratio_min": mesh.get(
                    "recording_duration_coverage_ratio_min", ""),
                "mesh_audio_callbacks_min": mesh.get("audio_callbacks_min", ""),
                "mesh_audio_callback_interval_avg_ms_max": mesh.get(
                    "audio_callback_interval_avg_ms_max", ""),
                "mesh_audio_callback_gap_over_2x_total": mesh.get("audio_callback_gap_over_2x_total", ""),
                "mesh_udp_sample_time_stale_rejects_total": mesh.get(
                    "udp_sample_time_stale_rejects_total", ""),
                "mesh_udp_sample_time_future_rejects_total": mesh.get(
                    "udp_sample_time_future_rejects_total", ""),
                "mesh_missing_audio_frames_total": mesh.get("missing_audio_frames_total", ""),
                "mesh_playback_dropped_frames_total": mesh.get("playback_dropped_frames_total", ""),
                "mesh_jitter_buffer_forced_releases_total": mesh.get(
                    "jitter_buffer_forced_releases_total", ""),
                "mesh_audio_tags": ";".join(mesh.get("audio_tags", [])),
                "server_metronome_wav_max_error_frames": wav_server.get("max_abs_error_frames", ""),
                "client_metronome_wav_max_error_frames": wav_client.get("max_abs_error_frames", ""),
                "server_metronome_wav_missing_clicks": wav_server.get("missing_clicks", ""),
                "client_metronome_wav_missing_clicks": wav_client.get("missing_clicks", ""),
                "server_csv_path": result.get("server_csv_path", ""),
                "client_csv_path": result.get("client_csv_path", ""),
            }
            row.update(combined)
            writer.writerow(row)
