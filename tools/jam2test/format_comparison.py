from __future__ import annotations

import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


METRIC_KEYS = (
    "udp_header_bytes",
    "audio_payload_bytes_max",
    "audio_packet_bytes_max",
    "network_audio_bytes_per_sample_max",
    "sent_packets_min",
    "recv_packets_min",
    "send_packet_rate_pps_max",
    "recv_packet_rate_pps_max",
    "sent_bytes_total",
    "recv_bytes_total",
    "send_bitrate_bps_max",
    "recv_bitrate_bps_max",
    "audio_callbacks_min",
    "audio_callback_interval_avg_ms_max",
    "audio_callback_interval_max_ms_max",
    "audio_callback_gap_over_2x_total",
    "loss_percent_max",
    "jitter_max_ms",
    "rtt_max_ms",
    "playback_underrun_time_ms_total",
    "playback_overruns_total",
    "playback_dropped_frames_total",
    "missing_audio_frames_total",
    "late_audio_frames_total",
    "drift_abs_ppm_max",
    "mix_capacity_drops_total",
)

REDUCTION_KEYS = (
    "audio_payload_bytes_max",
    "audio_packet_bytes_max",
    "sent_bytes_total",
    "recv_bytes_total",
    "send_bitrate_bps_max",
    "recv_bitrate_bps_max",
    "process_cpu_time_ms_total",
    "process_cpu_percent_one_core_mean",
)


def _mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def _numeric(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    return None


def _format_values(result: dict[str, Any]) -> dict[str, Any]:
    metrics = result.get("metrics", {})
    aggregate = (
        metrics.get("aggregate")
        or metrics.get("combined")
        or result.get("mesh_metrics", {})
    )
    values = {
        key: number
        for key in METRIC_KEYS
        if (number := _numeric(aggregate.get(key))) is not None
    }
    cpu_time: list[float] = []
    cpu_percent: list[float] = []
    wall_time: list[float] = []
    remote_rms: list[float] = []
    remote_peak: list[float] = []
    remote_pop_events = 0.0
    remote_clipped_frames = 0.0
    non_silent_remote_peers = 0
    analysis_ok = True
    analysis_observed = False
    peer_measurements = []
    for peer in result.get("peers", []):
        performance = (
            peer.get("performance")
            or peer.get("native_performance")
            or peer.get("native_manifest", {}).get("result", {})
        )
        measurement = {
            "machine_id": peer.get("machine_id", ""),
            "role": peer.get("role", ""),
        }
        for key, destination in (
            ("process_cpu_time_ms", cpu_time),
            ("process_cpu_percent_one_core", cpu_percent),
            ("wall_elapsed_ms", wall_time),
        ):
            number = _numeric(performance.get(key))
            if number is not None:
                destination.append(number)
                measurement[key] = number
        peer_measurements.append(measurement)
        analysis = peer.get("analysis") or peer.get("audio_analysis", {})
        if analysis:
            analysis_observed = True
            analysis_ok = analysis_ok and bool(analysis.get("ok", False))
        remote = analysis.get("stems", {}).get("their-input", {})
        rms = _numeric(remote.get("rms"))
        peak = _numeric(remote.get("peak"))
        if rms is not None:
            remote_rms.append(rms)
        if peak is not None:
            remote_peak.append(peak)
            non_silent_remote_peers += peak > 0.0
        remote_pop_events += _numeric(remote.get("pop_events")) or 0.0
        remote_clipped_frames += _numeric(remote.get("clipped_frames")) or 0.0
    values.update({
        "process_cpu_time_ms_total": sum(cpu_time),
        "process_cpu_percent_one_core_mean": _mean(cpu_percent),
        "process_cpu_percent_one_core_max": max(cpu_percent, default=0.0),
        "wall_elapsed_ms_max": max(wall_time, default=0.0),
        "remote_wav_rms_mean": _mean(remote_rms),
        "remote_wav_peak_max": max(remote_peak, default=0.0),
        "remote_wav_pop_events_total": remote_pop_events,
        "remote_wav_clipped_frames_total": remote_clipped_frames,
        "non_silent_remote_wav_peers": non_silent_remote_peers,
        "audio_analysis_observed": analysis_observed,
        "audio_analysis_ok": analysis_observed and analysis_ok,
        "peer_performance": peer_measurements,
    })
    return values


def _delta(pcm16: dict[str, Any], pcm24: dict[str, Any]) -> tuple[dict[str, float], dict[str, float]]:
    differences: dict[str, float] = {}
    reductions: dict[str, float] = {}
    for key in sorted(set(pcm16) & set(pcm24)):
        left = _numeric(pcm16.get(key))
        right = _numeric(pcm24.get(key))
        if left is None or right is None:
            continue
        differences[key] = left - right
        if key in REDUCTION_KEYS and right != 0.0:
            reductions[key] = (right - left) * 100.0 / right
    return differences, reductions


def format_comparison_summary(results: list[dict[str, Any]]) -> dict[str, Any]:
    grouped: dict[tuple[str, int], dict[str, dict[str, Any]]] = defaultdict(dict)
    for result in results:
        if result.get("verdict") not in ("complete", "pass", "expected_impairment"):
            continue
        if result.get("run_kind") not in (None, "", "primary"):
            continue
        case_id = str(result.get("case_id") or result.get("scenario") or "")
        if case_id.endswith("__pcm16"):
            base, audio_format = case_id[:-7], "pcm16"
        elif case_id.endswith("__pcm24"):
            base, audio_format = case_id[:-7], "pcm24"
        elif result.get("case", {}).get("category") == "wire-format":
            base = result["case"].get("comparator", case_id)
            audio_format = result["case"].get("network_audio_format", "")
        elif case_id == "control-fast-tone-pcm24":
            base, audio_format = case_id, "pcm24"
        else:
            continue
        grouped[(base, int(result.get("run_index", 0) or 0))][audio_format] = result

    pairs = []
    overall_values: dict[str, dict[str, list[float]]] = {
        "pcm16": defaultdict(list),
        "pcm24": defaultdict(list),
    }
    for (base_case_id, run_index), formats in sorted(grouped.items()):
        if set(formats) != {"pcm16", "pcm24"}:
            continue
        values = {name: _format_values(value) for name, value in formats.items()}
        differences, reductions = _delta(values["pcm16"], values["pcm24"])
        pairs.append({
            "base_case_id": base_case_id,
            "run_index": run_index,
            "formats": values,
            "pcm16_minus_pcm24": differences,
            "pcm16_reduction_percent": reductions,
        })
        for audio_format in ("pcm16", "pcm24"):
            for key, value in values[audio_format].items():
                number = _numeric(value)
                if number is not None:
                    overall_values[audio_format][key].append(number)

    overall = {
        audio_format: {key: _mean(values) for key, values in sorted(metrics.items())}
        for audio_format, metrics in overall_values.items()
    }
    differences, reductions = _delta(overall["pcm16"], overall["pcm24"])
    return {
        "schema": "jam2-format-comparison",
        "pair_count": len(pairs),
        "wire_protocol": {
            "version": 2,
            "header_bytes": 36,
            "replaced_header_bytes": 48,
            "header_reduction_percent": 25.0,
        },
        "pairs": pairs,
        "overall_mean": overall,
        "overall_pcm16_minus_pcm24": differences,
        "overall_pcm16_reduction_percent": reductions,
    }


def write_format_comparison(root: Path, results: list[dict[str, Any]]) -> dict[str, Any]:
    summary = format_comparison_summary(results)
    (root / "format-comparison.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    fields = [
        "base_case_id", "run_index", "pcm16_audio_packet_bytes", "pcm24_audio_packet_bytes",
        "packet_reduction_percent", "pcm16_send_bitrate_bps", "pcm24_send_bitrate_bps",
        "bitrate_reduction_percent", "pcm16_cpu_percent", "pcm24_cpu_percent",
        "cpu_reduction_percent", "pcm16_jitter_max_ms", "pcm24_jitter_max_ms",
        "pcm16_loss_percent", "pcm24_loss_percent", "pcm16_remote_wav_peak",
        "pcm24_remote_wav_peak", "pcm16_remote_wav_rms", "pcm24_remote_wav_rms",
    ]
    with (root / "format-comparison.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for pair in summary["pairs"]:
            pcm16 = pair["formats"]["pcm16"]
            pcm24 = pair["formats"]["pcm24"]
            reduction = pair["pcm16_reduction_percent"]
            writer.writerow({
                "base_case_id": pair["base_case_id"],
                "run_index": pair["run_index"],
                "pcm16_audio_packet_bytes": pcm16.get("audio_packet_bytes_max", ""),
                "pcm24_audio_packet_bytes": pcm24.get("audio_packet_bytes_max", ""),
                "packet_reduction_percent": reduction.get("audio_packet_bytes_max", ""),
                "pcm16_send_bitrate_bps": pcm16.get("send_bitrate_bps_max", ""),
                "pcm24_send_bitrate_bps": pcm24.get("send_bitrate_bps_max", ""),
                "bitrate_reduction_percent": reduction.get("send_bitrate_bps_max", ""),
                "pcm16_cpu_percent": pcm16.get("process_cpu_percent_one_core_mean", ""),
                "pcm24_cpu_percent": pcm24.get("process_cpu_percent_one_core_mean", ""),
                "cpu_reduction_percent": reduction.get("process_cpu_percent_one_core_mean", ""),
                "pcm16_jitter_max_ms": pcm16.get("jitter_max_ms", ""),
                "pcm24_jitter_max_ms": pcm24.get("jitter_max_ms", ""),
                "pcm16_loss_percent": pcm16.get("loss_percent_max", ""),
                "pcm24_loss_percent": pcm24.get("loss_percent_max", ""),
                "pcm16_remote_wav_peak": pcm16.get("remote_wav_peak_max", ""),
                "pcm24_remote_wav_peak": pcm24.get("remote_wav_peak_max", ""),
                "pcm16_remote_wav_rms": pcm16.get("remote_wav_rms_mean", ""),
                "pcm24_remote_wav_rms": pcm24.get("remote_wav_rms_mean", ""),
            })
    return summary
