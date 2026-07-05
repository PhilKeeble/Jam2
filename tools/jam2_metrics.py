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
        "elapsed_s": elapsed_s,
        "sent_packets": to_float(row, "sent_packets"),
        "recv_packets": recv_packets,
        "sequence_lost": sequence_lost,
        "sequence_loss_percent": loss_percent,
        "sequence_out_of_order": to_float(row, "sequence_out_of_order"),
        "sequence_duplicate": to_float(row, "sequence_duplicate"),
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
        "sample_time_playout": row.get("sample_time_playout", ""),
        "playout_delay_frames": to_float(row, "playout_delay_frames"),
        "playout_delay_ms": to_float(row, "playout_delay_ms"),
        "playout_delay_error_ms": to_float(row, "playout_delay_error_ms"),
        "socket_send_buffer_bytes": to_float(row, "socket_send_buffer_bytes"),
        "socket_recv_buffer_bytes": to_float(row, "socket_recv_buffer_bytes"),
        "requested_socket_send_buffer_bytes": to_float(row, "requested_socket_send_buffer_bytes"),
        "requested_socket_recv_buffer_bytes": to_float(row, "requested_socket_recv_buffer_bytes"),
        "requested_channels": row.get("requested_channels", ""),
        "active_channels": row.get("active_channels", ""),
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
        "elapsed_s": max((side.get("elapsed_s", 0.0) for side in sides), default=0.0),
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
        "sequence_lost_total": sum((side.get("sequence_lost", 0.0) for side in sides), 0.0),
        "sequence_out_of_order_total": sum((side.get("sequence_out_of_order", 0.0) for side in sides), 0.0),
        "reordered_recovered_total": sum((side.get("reordered_recovered", 0.0) for side in sides), 0.0),
        "reordered_lost_total": sum((side.get("reordered_lost", 0.0) for side in sides), 0.0),
        "drift_correction_clamped_percent_max": max((side.get("drift_correction_clamped_percent", 0.0) for side in sides), default=0.0),
        "resampler_ratio_min": min((side.get("resampler_ratio_min", 1.0) for side in sides), default=1.0),
        "resampler_ratio_max": max((side.get("resampler_ratio_max", 1.0) for side in sides), default=1.0),
        "playout_delay_error_ms_abs_max": max((abs(side.get("playout_delay_error_ms", 0.0)) for side in sides), default=0.0),
    }
    return {"server": server, "client": client, "combined": combined}


def write_results_csv(path, results):
    fields = [
        "scenario",
        "source_scenario",
        "profile_family",
        "profile",
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
        "sequence_lost_total",
        "sequence_out_of_order_total",
        "reordered_recovered_total",
        "reordered_lost_total",
        "drift_correction_clamped_percent_max",
        "resampler_ratio_min",
        "resampler_ratio_max",
        "playout_delay_error_ms_abs_max",
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
            row = {
                "scenario": result.get("scenario", ""),
                "source_scenario": result.get("source_scenario", result.get("scenario", "")),
                "profile_family": result.get("profile_family", ""),
                "profile": result.get("profile", ""),
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
                "server_csv_path": result.get("server_csv_path", ""),
                "client_csv_path": result.get("client_csv_path", ""),
            }
            row.update(combined)
            writer.writerow(row)
