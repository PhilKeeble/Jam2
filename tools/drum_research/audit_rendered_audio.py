#!/usr/bin/env python3
"""Report hard signal-health measurements for Jam2 corpus audition WAVs."""

from __future__ import annotations

import argparse
import json
import math
import struct
import wave
from collections import defaultdict
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "minimum": round(min(values), 6),
        "p05": round(percentile(values, 0.05), 6),
        "median": round(percentile(values, 0.50), 6),
        "p95": round(percentile(values, 0.95), 6),
        "maximum": round(max(values), 6),
        "mean": round(sum(values) / len(values), 6),
    }


def inspect_wav(path: Path) -> dict[str, float | int | str]:
    frames = 0
    square_sum = 0.0
    sample_sum = 0.0
    peak = 0.0
    near_limit = 0
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        sample_rate = source.getframerate()
        if channels != 1 or width != 2:
            raise ValueError(f"{path}: expected mono PCM16")
        while True:
            block = source.readframes(65536)
            if not block:
                break
            count = len(block) // 2
            samples = struct.unpack(f"<{count}h", block)
            frames += count
            for integer in samples:
                value = integer / 32768.0
                absolute = abs(value)
                peak = max(peak, absolute)
                square_sum += value * value
                sample_sum += value
                if absolute >= 0.979:
                    near_limit += 1
    rms = math.sqrt(square_sum / frames) if frames else 0.0
    crest_db = (
        20.0 * math.log10(peak / rms)
        if peak > 0.0 and rms > 0.0
        else 0.0
    )
    return {
        "file": path.name,
        "profile": path.name.split("__", 1)[0],
        "sampleRate": sample_rate,
        "frames": frames,
        "seconds": round(frames / sample_rate, 6),
        "peak": round(peak, 6),
        "peakDbfs": round(20.0 * math.log10(peak), 3)
        if peak
        else -120.0,
        "rms": round(rms, 6),
        "rmsDbfs": round(20.0 * math.log10(rms), 3)
        if rms
        else -120.0,
        "crestDb": round(crest_db, 3),
        "dcOffset": round(sample_sum / frames, 8) if frames else 0.0,
        "nearLimitSamples": near_limit,
        "nearLimitRatio": round(near_limit / frames, 9)
        if frames
        else 0.0,
    }


def audit(folder: Path) -> dict[str, object]:
    files = sorted(folder.glob("*.wav"))
    measurements = [inspect_wav(path) for path in files]
    by_profile: dict[str, list[dict[str, object]]] = defaultdict(list)
    for measurement in measurements:
        by_profile[str(measurement["profile"])].append(measurement)

    def aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
        return {
            "files": len(rows),
            "peak": summarize([float(row["peak"]) for row in rows]),
            "peakDbfs": summarize(
                [float(row["peakDbfs"]) for row in rows]
            ),
            "rmsDbfs": summarize(
                [float(row["rmsDbfs"]) for row in rows]
            ),
            "crestDb": summarize(
                [float(row["crestDb"]) for row in rows]
            ),
            "nearLimitSamples": sum(
                int(row["nearLimitSamples"]) for row in rows
            ),
            "maximumAbsoluteDcOffset": round(
                max(
                    (abs(float(row["dcOffset"])) for row in rows),
                    default=0.0,
                ),
                8,
            ),
        }

    return {
        "schema": "jam2-rendered-audio-audit-v1",
        "method": {
            "format": "mono PCM16",
            "nearLimitThreshold": 0.979,
            "note": (
                "Near-limit counts expose samples at Jam2's 0.98 "
                "PCM safety clamp; they are not inferred loudness scores."
            ),
        },
        "folder": str(folder),
        "overall": aggregate(measurements),
        "byProfile": {
            profile: aggregate(rows)
            for profile, rows in sorted(by_profile.items())
        },
        "files": measurements,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("folder", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(args.folder)
    encoded = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
