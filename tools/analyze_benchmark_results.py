#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Summarize static Jam2 benchmark results.")
    parser.add_argument("results", nargs="?", default=str(Path(__file__).with_name("benchmark_logs") / "benchmark_results.json"))
    parser.add_argument("--csv", default="")
    parser.add_argument("--text", default="")
    return parser.parse_args()


def load_results(path):
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    return payload.get("results", payload if isinstance(payload, list) else [])


def flatten(result):
    combined = result.get("metrics", {}).get("combined", {})
    profile = result.get("profile_values", {})
    return {
        "case_id": result.get("case_id", ""),
        "run_index": result.get("run_index", ""),
        "profile": result.get("profile", ""),
        "signal": result.get("signal", ""),
        "server_signal": result.get("server_signal", ""),
        "client_signal": result.get("client_signal", ""),
        "verdict": result.get("verdict", ""),
        "tags": ";".join(result.get("tags", [])),
        "audio_buffer_size": profile.get("audio_buffer_size", ""),
        "frame_size": profile.get("frame_size", ""),
        "playback_prefill_frames": profile.get("playback_prefill_frames", ""),
        "playback_ring_frames": profile.get("playback_ring_frames", ""),
        "playback_max_frames": profile.get("playback_max_frames", ""),
        "playout_delay_frames": profile.get("playout_delay_frames", ""),
        "adaptive_playback_cushion": profile.get("adaptive_playback_cushion", ""),
        "adaptive_playback_target_frames": profile.get("adaptive_playback_target_frames", ""),
        "adaptive_playback_min_frames": profile.get("adaptive_playback_min_frames", ""),
        "adaptive_playback_max_frames": profile.get("adaptive_playback_max_frames", ""),
        "jitter_buffer_frames": profile.get("jitter_buffer_frames", ""),
        "jitter_buffer_max_frames": profile.get("jitter_buffer_max_frames", ""),
        "loss_percent_max": combined.get("loss_percent_max", ""),
        "jitter_max_ms": combined.get("jitter_max_ms", ""),
        "rtt_max_ms": combined.get("rtt_max_ms", ""),
        "playback_underrun_time_ms_total": combined.get("playback_underrun_time_ms_total", ""),
        "playback_underrun_burst_max_ms": combined.get("playback_underrun_burst_max_ms", ""),
        "playback_dropped_frames_total": combined.get("playback_dropped_frames_total", ""),
        "missing_audio_frames_total": combined.get("missing_audio_frames_total", ""),
        "late_audio_frames_total": combined.get("late_audio_frames_total", ""),
        "drift_abs_ppm_max": combined.get("drift_abs_ppm_max", ""),
        "adaptive_raise_events_total": combined.get("adaptive_raise_events_total", ""),
        "adaptive_burst_events_total": combined.get("adaptive_burst_events_total", ""),
        "jitter_buffer_released_packets_total": combined.get("jitter_buffer_released_packets_total", ""),
        "jitter_buffer_dropped_packets_total": combined.get("jitter_buffer_dropped_packets_total", ""),
        "jitter_buffer_dropped_frames_total": combined.get("jitter_buffer_dropped_frames_total", ""),
        "jitter_buffer_depth_max_frames": combined.get("jitter_buffer_depth_max_frames", ""),
        "metronome_beat_delta_abs_max": combined.get("metronome_beat_delta_abs_max", ""),
        "server_csv_path": result.get("server_csv_path", ""),
        "client_csv_path": result.get("client_csv_path", ""),
    }


def write_csv(path, rows):
    fields = list(flatten({}).keys())
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(flatten(row))


def text_summary(rows):
    lines = []
    lines.append(f"{'case':44} {'profile':28} {'signal':22} {'S':9} {'C':9} {'run':>3} {'verdict':8} tags")
    lines.append("-" * 138)
    for row in rows:
        lines.append(
            f"{row.get('case_id','')[:44]:44} "
            f"{row.get('profile','')[:28]:28} "
            f"{row.get('signal','')[:22]:22} "
            f"{row.get('server_signal','')[:9]:9} "
            f"{row.get('client_signal','')[:9]:9} "
            f"{row.get('run_index', 0):>3} "
            f"{row.get('verdict',''):8} "
            f"{','.join(row.get('tags', [])) or '-'}")
    return "\n".join(lines)


def main():
    args = parse_args()
    rows = load_results(args.results)
    summary = text_summary(rows)
    print(summary)
    if args.csv:
        write_csv(args.csv, rows)
    if args.text:
        Path(args.text).write_text(summary + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
