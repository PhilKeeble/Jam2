#!/usr/bin/env python3
"""Report raw temporal, spectral, and stereo deltas for Ableton Pop A/B renders."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import wave

from audit_timbres import fft


FFT_SIZE = 8192
REFERENCE_NAMES = {
    "bass": "MelodicReference-Pop 808 Bass.wav",
    "chords": "MelodicReference-Pop Arcade Chords.wav",
    "melody": "MelodicReference-Pop Chime Melody.wav",
    "support": "MelodicReference-Pop Support Ballroom Blues.wav",
}
CONTEXT_NAMES = {
    "bass": "MelodicMix-Pop 808 Bass.wav",
    "chords": "MelodicMix-Pop Arcade Chords.wav",
    "melody": "MelodicMix-Pop Chime Melody.wav",
    "support": "MelodicMix-Pop Support Ballroom Blues.wav",
    "drums": "MelodicMix-Pop drums reference rock32.wav",
    "full-mix": "MelodicFullMix-Pop.wav",
}


def read_wav(path: Path) -> tuple[int, list[tuple[float, float]]]:
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels(); width = source.getsampwidth(); rate = source.getframerate()
        raw = source.readframes(source.getnframes())
    if channels not in (1, 2) or width not in (2, 3):
        raise ValueError(f"Expected mono/stereo PCM16/24: {path}")
    scale = float(1 << (width * 8 - 1))
    values = []
    for offset in range(0, len(raw), width):
        values.append(int.from_bytes(raw[offset:offset + width], "little", signed=True) / scale)
    if channels == 1:
        return rate, [(value, value) for value in values]
    return rate, [(values[index], values[index + 1]) for index in range(0, len(values), 2)]


def slice_seconds(samples, rate: int, start: float, duration: float):
    return samples[round(start * rate):round((start + duration) * rate)]


def mono(samples):
    return [(left + right) * .5 for left, right in samples]


def basic(samples) -> dict[str, float]:
    values = [value for frame in samples for value in frame]
    rms = math.sqrt(sum(value * value for value in values) / max(len(values), 1))
    peak = max((abs(value) for value in values), default=0)
    return {"peak": peak, "rms": rms, "crestDb": 20 * math.log10(max(peak, 1e-12) / max(rms, 1e-12))}


def stereo(samples) -> dict[str, float]:
    left = [frame[0] for frame in samples]; right = [frame[1] for frame in samples]
    mid = [(l + r) * .5 for l, r in samples]; side = [(l - r) * .5 for l, r in samples]
    def rms(values): return math.sqrt(sum(value * value for value in values) / max(len(values), 1))
    lr = sum(l * r for l, r in zip(left, right))
    ll = sum(l * l for l in left); rr = sum(r * r for r in right)
    return {
        "sideToMidDb": 20 * math.log10(max(rms(side), 1e-12) / max(rms(mid), 1e-12)),
        "correlation": lr / max(math.sqrt(ll * rr), 1e-12),
    }


def loudest_block(values: list[float]) -> list[float]:
    if len(values) <= FFT_SIZE:
        return values + [0] * (FFT_SIZE - len(values))
    best = max(range(0, len(values) - FFT_SIZE + 1, FFT_SIZE // 4), key=lambda start: sum(v * v for v in values[start:start + FFT_SIZE]))
    return values[best:best + FFT_SIZE]


def spectral(samples, rate: int, integrated: bool = False) -> dict[str, object]:
    values = mono(samples)
    blocks = [loudest_block(values)]
    if integrated and len(values) > FFT_SIZE:
        count = 24
        maximum = len(values) - FFT_SIZE
        starts = [round(maximum * index / (count - 1)) for index in range(count)]
        blocks = [values[start:start + FFT_SIZE] for start in starts]
    powers = [0.0] * (FFT_SIZE // 2)
    for block in blocks:
        windowed = [value * (.5 - .5 * math.cos(2 * math.pi * index / (FFT_SIZE - 1))) for index, value in enumerate(block)]
        spectrum = fft([complex(value) for value in windowed])
        for index, value in enumerate(spectrum[1:FFT_SIZE // 2 + 1]):
            powers[index] += abs(value) ** 2
    frequencies = [rate * index / FFT_SIZE for index in range(1, FFT_SIZE // 2 + 1)]
    total = max(sum(powers), 1e-24)
    bands = ((0, 80), (80, 250), (250, 1000), (1000, 4000), (4000, 16000))
    ratios = {f"{low}-{high}": sum(power for frequency, power in zip(frequencies, powers) if low <= frequency < high) / total for low, high in bands}
    centroid = sum(frequency * power for frequency, power in zip(frequencies, powers)) / total
    peaks = []
    for index in sorted(range(1, len(powers) - 1), key=lambda item: powers[item], reverse=True):
        if powers[index] < powers[index - 1] or powers[index] < powers[index + 1]:
            continue
        frequency = frequencies[index]
        if any(abs(frequency - previous) < 20 for previous in peaks):
            continue
        peaks.append(frequency)
        if len(peaks) == 6:
            break
    return {"centroidHz": centroid, "bandEnergyRatios": ratios, "strongestPeaksHz": peaks}


def envelope(samples, rate: int) -> dict[str, float]:
    values = mono(samples); block = max(1, round(.005 * rate))
    levels = [math.sqrt(sum(value * value for value in values[start:start + block]) / max(1, len(values[start:start + block]))) for start in range(0, len(values), block)]
    peak = max(levels, default=0); peak_index = levels.index(peak) if levels else 0
    result = {"peakBlockSeconds": peak_index * block / rate}
    for suffix, ratio in (("20", .1), ("40", .01)):
        after = [index for index in range(peak_index, len(levels)) if levels[index] >= peak * ratio]
        result[f"lastAboveMinus{suffix}DbSeconds"] = ((after[-1] + 1) * block / rate) if after else 0
    return result


def rounded(value):
    if isinstance(value, float): return round(value, 6)
    if isinstance(value, dict): return {key: rounded(item) for key, item in value.items()}
    return value


def metrics(samples, rate: int, integrated: bool = False) -> dict[str, object]:
    return rounded({"level": basic(samples), "stereo": stereo(samples), "spectral": spectral(samples, rate, integrated), "envelope": envelope(samples, rate)})


def delta(reference: dict, candidate: dict) -> dict:
    result = {}
    for group in ("stereo", "spectral", "envelope"):
        result[group] = {}
        for key, reference_value in reference[group].items():
            if isinstance(reference_value, dict):
                result[group][key] = {name: round(candidate[group][key][name] - value, 6) for name, value in reference_value.items()}
            elif isinstance(reference_value, list):
                continue
            else:
                result[group][key] = round(candidate[group][key] - reference_value, 6)
    return result


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--candidate", type=Path, default=root / "artifacts" / "melodic-pop-compare")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--roles", nargs="*", choices=tuple(REFERENCE_NAMES) + tuple(CONTEXT_NAMES))
    args = parser.parse_args()
    report: dict[str, object] = {"schema": "jam2-ableton-pop-comparison-v1", "isolated": {}, "context": {}}
    for role, filename in REFERENCE_NAMES.items():
        if args.roles and role not in args.roles:
            continue
        ref_rate, ref_all = read_wav(args.reference / filename)
        can_rate, can_all = read_wav(args.candidate / f"{role}-short.wav")
        reference_metrics = metrics(slice_seconds(ref_all, ref_rate, 66, 1.9), ref_rate)
        candidate_metrics = metrics(slice_seconds(can_all, can_rate, .2, 1.9), can_rate)
        report["isolated"][role] = {"reference": reference_metrics, "candidate": candidate_metrics, "delta": delta(reference_metrics, candidate_metrics)}
    for role, filename in CONTEXT_NAMES.items():
        if args.roles and role not in args.roles:
            continue
        ref_rate, reference = read_wav(args.reference / filename)
        candidate_name = "full-mix.wav" if role == "full-mix" else f"context/{role}.wav"
        can_rate, candidate = read_wav(args.candidate / candidate_name)
        reference_metrics = metrics(reference, ref_rate, True); candidate_metrics = metrics(candidate, can_rate, True)
        report["context"][role] = {"reference": reference_metrics, "candidate": candidate_metrics, "delta": delta(reference_metrics, candidate_metrics)}
    output = args.output or args.candidate / "comparison-report.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for role, item in report["isolated"].items():
        ref = item["reference"]; can = item["candidate"]
        print(f"{role:8s} centroid {ref['spectral']['centroidHz']:.0f}->{can['spectral']['centroidHz']:.0f} Hz, side/mid {ref['stereo']['sideToMidDb']:.1f}->{can['stereo']['sideToMidDb']:.1f} dB, t40 {ref['envelope']['lastAboveMinus40DbSeconds']:.2f}->{can['envelope']['lastAboveMinus40DbSeconds']:.2f}s")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
