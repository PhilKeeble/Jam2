#!/usr/bin/env python3

import argparse
import csv
from collections import defaultdict
from pathlib import Path


NUMERIC_FIELDS = [
    "elapsed_ms",
    "sequence_lost",
    "sequence_duplicate",
    "sequence_out_of_order",
    "sequence_late",
    "reordered_lost",
    "playback_dropped_frames",
    "playback_depth_avg_ms",
    "playback_depth_max_ms",
    "playback_ring_overruns",
    "playback_ring_underruns",
    "jitter_avg_ms",
    "jitter_max_ms",
    "rtt_avg_ms",
    "rtt_max_ms",
    "estimated_one_way_ms",
    "drift_ppm",
    "capture_ring_underruns",
]


def parse_args():
    parser = argparse.ArgumentParser(description="Rank Jam2 matrix profiles from a combined CSV.")
    parser.add_argument("--input", default=str(Path(__file__).with_name("logs") / "combined_stats.csv"))
    parser.add_argument("--output", default="")
    parser.add_argument("--max-loss", type=float, default=0.0)
    parser.add_argument("--max-playback-underruns", type=float, default=0.0)
    parser.add_argument("--max-playback-overruns", type=float, default=0.0)
    return parser.parse_args()


def to_float(row, field):
    value = row.get(field, "")
    if value == "":
        return 0.0
    try:
        return float(value)
    except ValueError:
        return 0.0


def read_rows(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def mean(values):
    return sum(values) / len(values) if values else 0.0


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * fraction))
    return ordered[index]


def profile_args(rows):
    row = rows[0]
    return {
        "audio_buffer_size": row.get("requested_audio_buffer_frames", ""),
        "frame_size": row.get("frame_size", ""),
        "prefill_frames": row.get("playback_prefill_frames", ""),
        "ring_frames": row.get("playback_ring_frames", ""),
        "max_frames": row.get("playback_max_frames", ""),
        "drift_max_ppm": row.get("drift_max_correction_ppm", ""),
    }


def aggregate_profile(test_id, rows, thresholds):
    values = {field: [to_float(row, field) for row in rows] for field in NUMERIC_FIELDS}
    sides = sorted(set(row.get("matrix_side", "") for row in rows))
    runs = sorted(set(row.get("matrix_run", "") for row in rows))
    stability_failures = (
        sum(values["sequence_lost"]) +
        sum(values["reordered_lost"]) +
        sum(values["playback_ring_overruns"]) +
        sum(values["playback_ring_underruns"]) +
        sum(values["playback_dropped_frames"])
    )
    stable = (
        sum(values["sequence_lost"]) <= thresholds["max_loss"] and
        sum(values["reordered_lost"]) <= thresholds["max_loss"] and
        sum(values["playback_ring_underruns"]) <= thresholds["max_playback_underruns"] and
        sum(values["playback_ring_overruns"]) <= thresholds["max_playback_overruns"] and
        sum(values["playback_dropped_frames"]) == 0
    )
    latency_avg = mean(values["estimated_one_way_ms"])
    latency_worst = max(values["estimated_one_way_ms"]) if values["estimated_one_way_ms"] else 0.0
    jitter_worst = max(values["jitter_max_ms"]) if values["jitter_max_ms"] else 0.0
    reorder_total = sum(values["sequence_out_of_order"])
    score = (
        stability_failures * 1000000.0 +
        reorder_total * 1000.0 +
        jitter_worst * 100.0 +
        latency_avg
    )
    result = {
        "matrix_test_id": test_id,
        "rank_score": score,
        "stable": "yes" if stable else "no",
        "row_count": len(rows),
        "sides": "+".join(sides),
        "runs": len(runs),
        "stability_failure_total": stability_failures,
        "sequence_lost_total": sum(values["sequence_lost"]),
        "reordered_lost_total": sum(values["reordered_lost"]),
        "out_of_order_total": reorder_total,
        "playback_underruns_total": sum(values["playback_ring_underruns"]),
        "playback_overruns_total": sum(values["playback_ring_overruns"]),
        "playback_dropped_frames_total": sum(values["playback_dropped_frames"]),
        "estimated_one_way_ms_avg": latency_avg,
        "estimated_one_way_ms_worst": latency_worst,
        "playback_depth_avg_ms_avg": mean(values["playback_depth_avg_ms"]),
        "playback_depth_max_ms_worst": max(values["playback_depth_max_ms"]) if values["playback_depth_max_ms"] else 0.0,
        "jitter_avg_ms_avg": mean(values["jitter_avg_ms"]),
        "jitter_max_ms_worst": jitter_worst,
        "rtt_avg_ms_avg": mean(values["rtt_avg_ms"]),
        "rtt_max_ms_worst": max(values["rtt_max_ms"]) if values["rtt_max_ms"] else 0.0,
        "drift_ppm_abs_avg": mean([abs(value) for value in values["drift_ppm"]]),
        "capture_underruns_total": sum(values["capture_ring_underruns"]),
    }
    result.update(profile_args(rows))
    return result


def write_analysis(path, rows):
    if not rows:
        return
    fieldnames = list(rows[0].keys())
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def print_summary(rows):
    stable_count = sum(1 for row in rows if row["stable"] == "yes")
    print("Analysis overview:")
    print(f"  profiles={len(rows)} stable={stable_count} unstable={len(rows) - stable_count}")

    def print_rows(title, ranked_rows):
        print(title)
        for index, row in enumerate(ranked_rows[:5], start=1):
            print(
                f"  {index}. {row['matrix_test_id']}: stable={row['stable']} "
                f"lat_avg={float(row['estimated_one_way_ms_avg']):.2f}ms "
                f"lat_worst={float(row['estimated_one_way_ms_worst']):.2f}ms "
                f"depth_avg={float(row['playback_depth_avg_ms_avg']):.2f}ms "
                f"jitter_max={float(row['jitter_max_ms_worst']):.2f}ms "
                f"loss={float(row['sequence_lost_total']):.0f} "
                f"reordered_lost={float(row['reordered_lost_total']):.0f} "
                f"underruns={float(row['playback_underruns_total']):.0f} "
                f"overruns={float(row['playback_overruns_total']):.0f}"
            )

    stable_rows = [row for row in rows if row["stable"] == "yes"]
    latency_rows = sorted(
        stable_rows if stable_rows else rows,
        key=lambda row: (float(row["estimated_one_way_ms_avg"]), float(row["estimated_one_way_ms_worst"])))
    stability_rows = sorted(
        rows,
        key=lambda row: (
            float(row["stability_failure_total"]),
            float(row["sequence_lost_total"]) + float(row["reordered_lost_total"]),
            float(row["playback_underruns_total"]) + float(row["playback_overruns_total"]),
            float(row["jitter_max_ms_worst"])))

    print_rows("Best overall profiles:", rows)
    print_rows("Lowest-latency stable profiles:", latency_rows)
    print_rows("Most-stable profiles:", stability_rows)

    if stable_rows:
        best = rows[0]
        print(
            "Recommended profile: "
            f"{best['matrix_test_id']} "
            f"(audio_buffer={best['audio_buffer_size']} frame={best['frame_size']} "
            f"prefill={best['prefill_frames']} ring={best['ring_frames']} max={best['max_frames']})"
        )
    else:
        best = stability_rows[0] if stability_rows else None
        if best is not None:
            print(
                "No fully stable profile found. Least-bad profile: "
                f"{best['matrix_test_id']} "
                f"(failures={float(best['stability_failure_total']):.0f}, "
                f"lat_avg={float(best['estimated_one_way_ms_avg']):.2f}ms)"
            )


def print_legacy_summary(rows):
    print("Top profiles:")
    for row in rows[:5]:
        print(
            f"{row['matrix_test_id']}: stable={row['stable']} "
            f"score={float(row['rank_score']):.1f} "
            f"lat_avg={float(row['estimated_one_way_ms_avg']):.2f}ms "
            f"lat_worst={float(row['estimated_one_way_ms_worst']):.2f}ms "
            f"loss={float(row['sequence_lost_total']):.0f} "
            f"reordered_lost={float(row['reordered_lost_total']):.0f} "
            f"underruns={float(row['playback_underruns_total']):.0f} "
            f"overruns={float(row['playback_overruns_total']):.0f}"
        )


def analyze_matrix_csv(
    input_path,
    output_path=None,
    max_loss=0.0,
    max_playback_underruns=0.0,
    max_playback_overruns=0.0,
    print_top=True):
    input_path = Path(input_path)
    if not input_path.exists():
        raise SystemExit(f"combined CSV not found: {input_path}")
    output_path = Path(output_path) if output_path else input_path.with_name("analysis.csv")
    rows = read_rows(input_path)
    grouped = defaultdict(list)
    for row in rows:
        if row.get("row_type") != "final":
            continue
        grouped[row.get("matrix_test_id", "")].append(row)
    thresholds = {
        "max_loss": max_loss,
        "max_playback_underruns": max_playback_underruns,
        "max_playback_overruns": max_playback_overruns,
    }
    analysis = [aggregate_profile(test_id, profile_rows, thresholds) for test_id, profile_rows in grouped.items()]
    analysis.sort(key=lambda row: (row["stable"] != "yes", row["rank_score"], row["estimated_one_way_ms_avg"]))
    write_analysis(output_path, analysis)
    if print_top:
        print(f"wrote {len(analysis)} profile row(s) to {output_path}")
        print_summary(analysis)
    return output_path, analysis


def main():
    args = parse_args()
    analyze_matrix_csv(
        args.input,
        args.output or None,
        args.max_loss,
        args.max_playback_underruns,
        args.max_playback_overruns,
        print_top=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
