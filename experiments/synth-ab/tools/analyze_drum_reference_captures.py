#!/usr/bin/env python3
"""Measure standardized isolated drum-reference captures without dependencies."""

from __future__ import annotations

import argparse
import audioop
import json
import math
import statistics
import sys
import wave
from pathlib import Path

from audit_timbres import fft
from generate_drum_capture_midi import capture_velocities


FFT_SIZE = 8192
FIRST_HIT_SECONDS = 2.0
CAPTURES = (
    ("kick", "DrumSamples Kick.wav", 1.0, "Kick-808-Tone6-q"),
    ("snare", "DrumSamples Snare.wav", 1.0, "Snare-808-Tone3-r"),
    ("closed-hat", "DrumSamples Closed Hats.wav", 1.0, "Hihat-808-Closed"),
    ("open-hat", "DrumSamples Open Hats.wav", 4.0, "Hihat-808-Open"),
    ("high-tom", "DrumSamples Hi Tom.wav", 1.0, "Conga-808-Low"),
    ("mid-tom", "DrumSamples Mid Tom.wav", 1.0, "Tom-808-Mid"),
    ("floor-tom", "DrumSamples Low Tom.wav", 1.0, "Tom-808-Low"),
    ("crash", "DrumSamples Cymbals.wav", 4.0, "Cymbal-808-Soft"),
    ("ride", "DrumSamples ride.wav", 4.0, "Cowbell-808"),
    ("cross-stick", "DrumSamples cross stick.wav", 1.0, "Rim-808"),
)
VELOCITIES = capture_velocities()
SECTION_RANGES = {
    "ladder": range(0, 17),
    "ghost": range(17, 29),
    "soft": range(29, 41),
    "medium": range(41, 53),
    "hard": range(53, 65),
}


def decode_pcm(payload: bytes, width: int, channels: int) -> list[float]:
    full_scale = float((1 << (width * 8 - 1)) - 1)
    frame_width = width * channels
    samples: list[float] = []
    for frame_start in range(0, len(payload) - frame_width + 1, frame_width):
        total = 0
        for channel in range(channels):
            start = frame_start + channel * width
            total += int.from_bytes(payload[start : start + width], "little", signed=True)
        samples.append(total / channels / full_scale)
    return samples


def spectral_metrics(samples: list[float], rate: int) -> dict[str, object]:
    block = samples[:FFT_SIZE]
    if len(block) < FFT_SIZE:
        block = block + [0.0] * (FFT_SIZE - len(block))
    windowed = [
        value * (0.5 - 0.5 * math.cos(2.0 * math.pi * index / (FFT_SIZE - 1)))
        for index, value in enumerate(block)
    ]
    spectrum = fft([complex(value, 0.0) for value in windowed])
    powers = [abs(value) ** 2 for value in spectrum[: FFT_SIZE // 2 + 1]]
    frequencies = [rate * index / FFT_SIZE for index in range(len(powers))]
    audible_total = max(sum(powers[1:]), 1.0e-24)
    centroid = sum(
        frequency * power for frequency, power in zip(frequencies[1:], powers[1:])
    ) / audible_total
    cumulative = 0.0
    rolloff = frequencies[-1]
    for frequency, power in zip(frequencies[1:], powers[1:]):
        cumulative += power
        if cumulative >= 0.85 * audible_total:
            rolloff = frequency
            break
    bands = ((0, 80), (80, 250), (250, 2000), (2000, 8000), (8000, 24000))
    band_ratios = {}
    for low, high in bands:
        energy = sum(
            power
            for frequency, power in zip(frequencies[1:], powers[1:])
            if low <= frequency < min(high, rate / 2)
        )
        band_ratios[f"{low}-{high}"] = energy / audible_total

    candidates: list[tuple[float, float]] = []
    upper = min(len(powers) - 1, int(16000 * FFT_SIZE / rate))
    for index in range(2, upper):
        if powers[index] > powers[index - 1] and powers[index] >= powers[index + 1]:
            candidates.append((powers[index], frequencies[index]))
    selected: list[dict[str, float]] = []
    maximum_candidate_power = max((power for power, _frequency in candidates), default=1.0e-24)
    for power, frequency in sorted(candidates, reverse=True):
        if any(abs(frequency - item["frequencyHz"]) < 20.0 for item in selected):
            continue
        selected.append(
            {
                "frequencyHz": round(frequency, 2),
                "relativeDb": round(10.0 * math.log10(max(power / maximum_candidate_power, 1.0e-12)), 2),
            }
        )
        if len(selected) == 8:
            break
    return {
        "centroidHz": round(centroid, 2),
        "rolloff85Hz": round(rolloff, 2),
        "bandEnergyRatios": {key: round(value, 6) for key, value in band_ratios.items()},
        "strongestPeaks": selected,
    }


def temporal_metrics(samples: list[float], rate: int) -> dict[str, float]:
    if not samples:
        return {}
    peak = max(abs(value) for value in samples)
    peak_frame = max(range(len(samples)), key=lambda index: abs(samples[index]))
    squares = [value * value for value in samples]
    total_energy = max(sum(squares), 1.0e-24)
    cumulative = 0.0
    energy95_frame = len(samples) - 1
    for index, energy in enumerate(squares):
        cumulative += energy
        if cumulative >= 0.95 * total_energy:
            energy95_frame = index
            break

    block_frames = max(1, round(rate * 0.005))
    block_rms = []
    for start in range(0, len(samples), block_frames):
        block = samples[start : start + block_frames]
        block_rms.append(math.sqrt(sum(value * value for value in block) / max(len(block), 1)))
    maximum_rms = max(block_rms, default=0.0)
    threshold = maximum_rms * 0.01
    last_above = max((index for index, value in enumerate(block_rms) if value >= threshold), default=0)
    return {
        "peak": round(peak, 8),
        "peakTimeMs": round(1000.0 * peak_frame / rate, 3),
        "energy95Seconds": round(energy95_frame / rate, 4),
        "lastAboveMinus40DbSeconds": round((last_above + 1) * block_frames / rate, 4),
    }


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def analyze_capture(root: Path, piece: str, filename: str, interval: float, source_sample: str) -> dict[str, object]:
    path = root / filename
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        rate = source.getframerate()
        frame_count = source.getnframes()
        compression = source.getcomptype()
        if compression != "NONE" or width not in (2, 3) or channels not in (1, 2):
            raise ValueError(f"{path}: expected mono/stereo PCM16 or PCM24")
        peaks = []
        segments: list[bytes] = []
        frames_per_hit = round(interval * rate)
        for index in range(len(VELOCITIES)):
            start = round((FIRST_HIT_SECONDS + index * interval) * rate)
            if start >= frame_count:
                raise ValueError(f"{path}: capture is missing hit {index + 1}")
            source.setpos(start)
            available_frames = min(frames_per_hit, frame_count - start)
            payload = source.readframes(available_frames)
            if available_frames < frames_per_hit:
                payload += bytes((frames_per_hit - available_frames) * width * channels)
            segments.append(payload)
            peaks.append(audioop.max(payload, width) / ((1 << (width * 8 - 1)) - 1))

    hard_indices = list(SECTION_RANGES["hard"])
    hard_median = median([peaks[index] for index in hard_indices])
    complete_hard_indices = [
        index
        for index in hard_indices
        if FIRST_HIT_SECONDS + (index + 1) * interval <= frame_count / rate
    ]
    representative_index = min(
        complete_hard_indices or hard_indices,
        key=lambda index: abs(peaks[index] - hard_median),
    )
    representative = decode_pcm(segments[representative_index], width, channels)
    section_peaks = {
        name: median([peaks[index] for index in indices])
        for name, indices in SECTION_RANGES.items()
        if name != "ladder"
    }
    hard_peak = max(section_peaks["hard"], 1.0e-12)
    velocity_ratios = {
        name: round(value / hard_peak, 6)
        for name, value in section_peaks.items()
    }
    ladder = [
        {"velocity": velocity, "peak": round(peaks[index], 8)}
        for index, velocity in enumerate(VELOCITIES[:17])
    ]
    return {
        "piece": piece,
        "file": filename,
        "sourceSample": source_sample,
        "format": {
            "sampleRate": rate,
            "channels": channels,
            "bitDepth": width * 8,
            "durationSeconds": round(frame_count / rate, 6),
        },
        "capture": {
            "firstHitSeconds": FIRST_HIT_SECONDS,
            "intervalSeconds": interval,
            "hitCount": len(VELOCITIES),
            "representativeHardHit": representative_index + 1,
        },
        "velocity": {
            "medianSectionPeaks": {key: round(value, 8) for key, value in section_peaks.items()},
            "gainRelativeToHard": velocity_ratios,
            "ladder": ladder,
        },
        "temporal": temporal_metrics(representative, rate),
        "spectral": spectral_metrics(representative, rate),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="directory containing standardized drum WAVs")
    parser.add_argument("--output", type=Path, help="optional JSON output path")
    parser.add_argument(
        "--pieces",
        nargs="*",
        choices=[spec[0] for spec in CAPTURES],
        help="analyze only these pieces",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    selected = set(args.pieces or ())
    captures = [
        analyze_capture(root, *spec)
        for spec in CAPTURES
        if not selected or spec[0] in selected
    ]
    report = {
        "schema": "jam2-drum-reference-analysis-v1",
        "sourceRoot": str(root),
        "captures": captures,
    }
    serialized = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    else:
        sys.stdout.write(serialized)
    for capture in captures:
        temporal = capture["temporal"]
        spectral = capture["spectral"]
        print(
            f"{capture['piece']:16s} peak={temporal['peak']:.4f} "
            f"t40={temporal['lastAboveMinus40DbSeconds']:.3f}s "
            f"centroid={spectral['centroidHz']:.0f}Hz "
            f"peaks={','.join(str(item['frequencyHz']) for item in spectral['strongestPeaks'][:4])}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
