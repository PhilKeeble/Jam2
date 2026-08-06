#!/usr/bin/env python3
"""Compare the four standardized 808 pattern windows and silent gaps."""

from __future__ import annotations

import argparse
import json
import math
import wave
from pathlib import Path

from analyze_drum_reference_captures import decode_pcm


WINDOWS = (
    ("pattern-1", 0.0, 16.0),
    ("gap-1", 16.0, 20.0),
    ("pattern-2", 20.0, 36.0),
    ("gap-2", 36.0, 40.0),
    ("pattern-3", 40.0, 56.0),
    ("gap-3", 56.0, 60.0),
    ("pattern-4", 60.0, 76.0),
    ("tail", 76.0, 80.0),
)


def load_mono(path: Path) -> tuple[int, list[float], dict[str, int]]:
    with wave.open(str(path), "rb") as source:
        rate = source.getframerate()
        channels = source.getnchannels()
        width = source.getsampwidth()
        frames = source.getnframes()
        samples = decode_pcm(source.readframes(frames), width, channels)
    return rate, samples, {"channels": channels, "bitDepth": width * 8, "frames": frames}


def level(samples: list[float]) -> dict[str, float]:
    if not samples:
        return {"peak": 0.0, "peakDbfs": -240.0, "rms": 0.0, "rmsDbfs": -240.0}
    peak = max(abs(value) for value in samples)
    rms = math.sqrt(sum(value * value for value in samples) / len(samples))
    return {
        "peak": round(peak, 8),
        "peakDbfs": round(20.0 * math.log10(max(peak, 1.0e-12)), 3),
        "rms": round(rms, 8),
        "rmsDbfs": round(20.0 * math.log10(max(rms, 1.0e-12)), 3),
    }


def analyze(path: Path) -> dict[str, object]:
    rate, samples, wave_format = load_mono(path)
    windows = []
    for name, start, end in WINDOWS:
        window = samples[round(start * rate) : round(end * rate)]
        windows.append({"name": name, "startSeconds": start, "endSeconds": end, **level(window)})
    return {
        "path": str(path.resolve()),
        "sampleRate": rate,
        **wave_format,
        "durationSeconds": round(len(samples) / rate, 6),
        "wholeFile": level(samples),
        "windows": windows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("generated", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {
        "schema": "jam2-ableton808-pattern-suite-comparison-v1",
        "source": analyze(args.source),
        "generated": analyze(args.generated),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    source_windows = report["source"]["windows"]
    generated_windows = report["generated"]["windows"]
    for source, generated in zip(source_windows, generated_windows):
        print(
            f"{source['name']:<10} source={source['rmsDbfs']:>7.3f} dBFS "
            f"generated={generated['rmsDbfs']:>7.3f} dBFS "
            f"delta={generated['rmsDbfs'] - source['rmsDbfs']:+7.3f} dB"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
