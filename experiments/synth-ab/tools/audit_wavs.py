#!/usr/bin/env python3
"""Measure generated workbench stems and their default dry scene balances."""

from __future__ import annotations

import argparse
import array
from concurrent.futures import ProcessPoolExecutor
import json
import math
import os
import sys
import wave
from pathlib import Path


TARGET_RMS = {
    "chords": 0.12,
    "melody": 0.12,
    "bass": 0.14,
    "support": 0.09,
    "drums": 0.17,
}

DEFAULT_LEVEL = {
    "chords": 82,
    "melody": 82,
    "bass": 82,
    "support": 68,
    "drums": 82,
}

MASTER_GAIN = 0.42


def load_pcm16(path: Path) -> tuple[int, array.array]:
    with wave.open(str(path), "rb") as source:
        if (
            source.getnchannels() != 1
            or source.getsampwidth() != 2
            or source.getcomptype() != "NONE"
        ):
            raise ValueError(f"{path}: expected mono PCM16")
        rate = source.getframerate()
        samples = array.array("h")
        samples.frombytes(source.readframes(source.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    return rate, samples


def stem_metrics(rate: int, samples: array.array) -> dict[str, float | int]:
    count = len(samples)
    if not count:
        return {
            "frames": 0,
            "seconds": 0.0,
            "peak": 0.0,
            "rms": 0.0,
            "dc": 0.0,
            "crest_db": 0.0,
            "zcr": 0.0,
            "difference_ratio": 0.0,
            "ceiling_samples": 0,
            "silent_windows": 0,
        }
    sum_value = 0
    sum_squares = 0
    sum_difference_squares = 0
    peak = 0
    crossings = 0
    previous = samples[0]
    previous_sign = previous >= 0
    ceiling_samples = 0
    for sample in samples:
        magnitude = abs(sample)
        peak = max(peak, magnitude)
        sum_value += sample
        sum_squares += sample * sample
        difference = sample - previous
        sum_difference_squares += difference * difference
        sign = sample >= 0
        if sign != previous_sign:
            crossings += 1
        previous = sample
        previous_sign = sign
        if magnitude >= 32760:
            ceiling_samples += 1
    rms_integer = math.sqrt(sum_squares / count)
    rms = rms_integer / 32768.0
    difference_rms = math.sqrt(sum_difference_squares / count) / 32768.0
    window = max(1, rate // 20)
    silent_windows = 0
    active_windows = 0
    window_rms_values: list[float] = []
    for start in range(0, count, window):
        values = samples[start : min(start + window, count)]
        value = math.sqrt(sum(sample * sample for sample in values) / len(values))
        normalized = value / 32768.0
        window_rms_values.append(normalized)
        if normalized < 0.001:
            silent_windows += 1
        else:
            active_windows += 1
    quarter = max(1, len(window_rms_values) // 4)
    first_rms = sum(window_rms_values[:quarter]) / quarter
    last_rms = sum(window_rms_values[-quarter:]) / quarter
    return {
        "frames": count,
        "seconds": count / rate,
        "peak": peak / 32768.0,
        "rms": rms,
        "dc": sum_value / count / 32768.0,
        "crest_db": 20.0 * math.log10(max(peak / 32768.0, 1e-12) / max(rms, 1e-12)),
        "zcr": crossings / count,
        "difference_ratio": difference_rms / max(rms, 1e-12),
        "ceiling_samples": ceiling_samples,
        "silent_windows": silent_windows,
        "active_windows": active_windows,
        "first_last_rms_ratio": last_rms / max(first_rms, 1e-12),
    }


def shaped(value: float) -> float:
    # The browser's zero-drive waveshaper is an identity curve.
    return value


def level_gain(role: str) -> float:
    value = DEFAULT_LEVEL[role] / 100.0
    return value**1.35 * 1.05


def scene_metrics(stems: dict[str, array.array]) -> dict[str, float | int]:
    frames = max((len(samples) for samples in stems.values()), default=0)
    peak = 0.0
    sum_squares = 0.0
    overload_samples = 0
    for index in range(frames):
        mixed = 0.0
        for role, samples in stems.items():
            value = samples[index] / 32768.0 if index < len(samples) else 0.0
            mixed += shaped(value) * level_gain(role) * MASTER_GAIN
        peak = max(peak, abs(mixed))
        sum_squares += mixed * mixed
        if abs(mixed) > 1.0:
            overload_samples += 1
    return {
        "peak": peak,
        "rms": math.sqrt(sum_squares / max(frames, 1)),
        "overload_samples": overload_samples,
        "overload_percent": 100.0 * overload_samples / max(frames, 1),
    }


def rms_envelope(samples: array.array, window: int) -> list[float]:
    result: list[float] = []
    for start in range(0, len(samples), window):
        values = samples[start : min(start + window, len(samples))]
        result.append(math.sqrt(sum(value * value for value in values) / len(values)))
    return result


def correlation(left: list[float], right: list[float]) -> float:
    count = min(len(left), len(right))
    if count < 2:
        return 0.0
    left = left[:count]
    right = right[:count]
    left_mean = sum(left) / count
    right_mean = sum(right) / count
    numerator = sum(
        (a - left_mean) * (b - right_mean) for a, b in zip(left, right)
    )
    left_energy = sum((value - left_mean) ** 2 for value in left)
    right_energy = sum((value - right_mean) ** 2 for value in right)
    return numerator / math.sqrt(max(left_energy * right_energy, 1e-24))


def pair_metrics(
    rate: int, jam2: array.array, daisy: array.array
) -> dict[str, float]:
    window = max(1, rate // 200)  # 5 ms
    left = rms_envelope(jam2, window)
    right = rms_envelope(daisy, window)
    maximum_lag = 4  # +/-20 ms
    candidates: list[tuple[float, int]] = []
    for lag in range(-maximum_lag, maximum_lag + 1):
        if lag < 0:
            value = correlation(left[-lag:], right[:lag])
        elif lag > 0:
            value = correlation(left[:-lag], right[lag:])
        else:
            value = correlation(left, right)
        candidates.append((value, lag))
    best, lag = max(candidates)
    return {
        "envelope_correlation_zero_lag": correlation(left, right),
        "envelope_correlation_best": best,
        "daisy_lag_ms_at_best_correlation": 1000.0 * lag * window / rate,
    }


def analyze_profile(profile_dir: Path) -> dict[str, object]:
    stems_by_engine: dict[str, dict[str, array.array]] = {
        "jam2": {},
        "daisy": {},
        "hybrid": {},
    }
    stem_results: list[dict[str, object]] = []
    sample_rate = 0
    for path in sorted(profile_dir.glob("*.wav")):
        role, engine = path.stem.rsplit("-", 1)
        rate, samples = load_pcm16(path)
        sample_rate = rate
        stems_by_engine[engine][role] = samples
        metrics = stem_metrics(rate, samples)
        metrics.update(
            {
                "role": role,
                "engine": engine,
                "target_rms": TARGET_RMS[role],
                "rms_shortfall": TARGET_RMS[role] - float(metrics["rms"]),
            }
        )
        stem_results.append(metrics)
    scenes = {
        engine: scene_metrics(stems)
        for engine, stems in stems_by_engine.items()
        if stems
    }
    comparisons = {
        role: pair_metrics(
            sample_rate,
            stems_by_engine["jam2"][role],
            stems_by_engine["daisy"][role],
        )
        for role in stems_by_engine["jam2"].keys()
        & stems_by_engine["daisy"].keys()
    }
    return {
        "profile": profile_dir.name,
        "sample_rate": sample_rate,
        "stems": stem_results,
        "scenes": scenes,
        "jam2_daisy_comparisons": comparisons,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "site" / "audio" / "designs",
    )
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(4, os.cpu_count() or 1),
        help="Profile-analysis worker processes (default: up to 4).",
    )
    args = parser.parse_args()
    profile_dirs = sorted(
        path for path in args.root.iterdir() if path.is_dir()
    )
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    if args.jobs == 1:
        profiles = [analyze_profile(path) for path in profile_dirs]
    else:
        with ProcessPoolExecutor(
            max_workers=min(args.jobs, len(profile_dirs) or 1)
        ) as executor:
            profiles = list(executor.map(analyze_profile, profile_dirs))
    if args.json:
        print(json.dumps({"profiles": profiles}, indent=2))
        return 0
    for profile in profiles:
        flags: list[str] = []
        stems = profile["stems"]
        assert isinstance(stems, list)
        for stem in stems:
            assert isinstance(stem, dict)
            if stem["ceiling_samples"]:
                flags.append(
                    f"{stem['role']}-{stem['engine']}:ceiling={stem['ceiling_samples']}"
                )
            if float(stem["rms_shortfall"]) > 0.035:
                flags.append(
                    f"{stem['role']}-{stem['engine']}:rms={stem['rms']:.3f}"
                )
            if abs(float(stem["dc"])) > 0.005:
                flags.append(f"{stem['role']}-{stem['engine']}:dc={stem['dc']:.4f}")
        scenes = profile["scenes"]
        assert isinstance(scenes, dict)
        scene_text = " ".join(
            f"{engine}:peak={values['peak']:.2f}/over={values['overload_percent']:.2f}%"
            for engine, values in scenes.items()
        )
        print(
            f"{profile['profile']:<28} {scene_text}"
            + (f" LEVEL_NOTES {'; '.join(flags)}" if flags else "")
        )
    all_stems = [
        stem
        for profile in profiles
        for stem in profile["stems"]
    ]
    all_scenes = [
        scene
        for profile in profiles
        for scene in profile["scenes"].values()
    ]
    comparisons = [
        comparison
        for profile in profiles
        for comparison in profile["jam2_daisy_comparisons"].values()
    ]
    print(
        "SUMMARY"
        f" profiles={len(profiles)}"
        f" stems={len(all_stems)}"
        f" ceiling_samples={sum(int(stem['ceiling_samples']) for stem in all_stems)}"
        f" max_abs_dc={max((abs(float(stem['dc'])) for stem in all_stems), default=0.0):.6f}"
        f" scenes={len(all_scenes)}"
        f" overloaded_scenes={sum(float(scene['overload_percent']) > 0.0 for scene in all_scenes)}"
        f" max_scene_peak={max((float(scene['peak']) for scene in all_scenes), default=0.0):.3f}"
        f" jam2_daisy_pairs={len(comparisons)}"
        f" natural_envelope_lag_range_ms="
        f"{min((float(pair['daisy_lag_ms_at_best_correlation']) for pair in comparisons), default=0.0):.1f}.."
        f"{max((float(pair['daisy_lag_ms_at_best_correlation']) for pair in comparisons), default=0.0):.1f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
