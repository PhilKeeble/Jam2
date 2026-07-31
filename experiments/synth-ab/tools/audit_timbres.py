#!/usr/bin/env python3
"""Report raw spectral and transient measurements for workbench Daisy stems.

This is a diagnostic, not a perceptual quality score. Notes, voicings and
rhythmic density affect the measurements as well as the patch architecture.
"""

from __future__ import annotations

import argparse
import array
from collections import Counter
import cmath
import json
import math
from pathlib import Path
import statistics
import sys
import wave


FFT_SIZE = 4096
FEATURE_NAMES = (
    "centroid_hz",
    "rolloff_85_hz",
    "flatness",
    "low_ratio",
    "body_ratio",
    "presence_ratio",
    "air_ratio",
    "zcr",
    "difference_ratio",
    "crest_db",
)


def load_manifest(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    payload = text[text.index("=") + 1 :].strip().removesuffix(";")
    return json.loads(payload)


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


def loudest_block(samples: array.array, size: int) -> list[float]:
    if len(samples) <= size:
        result = [sample / 32768.0 for sample in samples]
        return result + [0.0] * (size - len(result))
    best_start = 0
    best_energy = -1
    for start in range(0, len(samples) - size + 1, size):
        energy = sum(
            sample * sample for sample in samples[start : start + size]
        )
        if energy > best_energy:
            best_energy = energy
            best_start = start
    return [
        sample / 32768.0
        for sample in samples[best_start : best_start + size]
    ]


def fft(values: list[complex]) -> list[complex]:
    count = len(values)
    result = list(values)
    target = 0
    for source in range(1, count):
        bit = count >> 1
        while target & bit:
            target ^= bit
            bit >>= 1
        target ^= bit
        if source < target:
            result[source], result[target] = result[target], result[source]
    length = 2
    while length <= count:
        step = cmath.exp(-2j * math.pi / length)
        half = length // 2
        for start in range(0, count, length):
            phase = 1.0 + 0.0j
            for offset in range(half):
                even = result[start + offset]
                odd = result[start + offset + half] * phase
                result[start + offset] = even + odd
                result[start + offset + half] = even - odd
                phase *= step
        length *= 2
    return result


def timbre_metrics(rate: int, samples: array.array) -> dict[str, float]:
    block = loudest_block(samples, FFT_SIZE)
    windowed = [
        value * (0.5 - 0.5 * math.cos(2.0 * math.pi * index / (FFT_SIZE - 1)))
        for index, value in enumerate(block)
    ]
    spectrum = fft([complex(value, 0.0) for value in windowed])
    powers = [
        abs(value) ** 2 for value in spectrum[1 : FFT_SIZE // 2 + 1]
    ]
    frequencies = [
        rate * index / FFT_SIZE for index in range(1, FFT_SIZE // 2 + 1)
    ]
    total = max(sum(powers), 1.0e-24)
    centroid = sum(
        frequency * power for frequency, power in zip(frequencies, powers)
    ) / total
    cumulative = 0.0
    rolloff = frequencies[-1]
    for frequency, power in zip(frequencies, powers):
        cumulative += power
        if cumulative >= 0.85 * total:
            rolloff = frequency
            break
    audible = [
        power
        for frequency, power in zip(frequencies, powers)
        if 30.0 <= frequency <= 16000.0
    ]
    arithmetic = sum(audible) / max(len(audible), 1)
    geometric = math.exp(
        sum(math.log(max(power, 1.0e-24)) for power in audible)
        / max(len(audible), 1)
    )

    def band(first: float, last: float) -> float:
        return sum(
            power
            for frequency, power in zip(frequencies, powers)
            if first <= frequency < last
        ) / total

    rms = math.sqrt(sum(value * value for value in block) / len(block))
    peak = max(abs(value) for value in block)
    differences = [
        block[index] - block[index - 1]
        for index in range(1, len(block))
    ]
    difference_rms = math.sqrt(
        sum(value * value for value in differences)
        / max(len(differences), 1)
    )
    crossings = sum(
        (block[index] >= 0.0) != (block[index - 1] >= 0.0)
        for index in range(1, len(block))
    )
    return {
        "centroid_hz": centroid,
        "rolloff_85_hz": rolloff,
        "flatness": geometric / max(arithmetic, 1.0e-24),
        "low_ratio": band(0.0, 250.0),
        "body_ratio": band(250.0, 1000.0),
        "presence_ratio": band(1000.0, 4000.0),
        "air_ratio": band(4000.0, 16000.0),
        "zcr": crossings / max(len(block) - 1, 1),
        "difference_ratio": difference_rms / max(rms, 1.0e-12),
        "crest_db": 20.0
        * math.log10(max(peak, 1.0e-12) / max(rms, 1.0e-12)),
    }


def standardized_neighbours(
    records: list[dict[str, object]],
) -> list[tuple[float, str, str]]:
    means = {
        name: statistics.mean(float(record[name]) for record in records)
        for name in FEATURE_NAMES
    }
    deviations = {
        name: statistics.pstdev(float(record[name]) for record in records)
        for name in FEATURE_NAMES
    }
    pairs: list[tuple[float, str, str]] = []
    for left_index, left in enumerate(records):
        for right in records[left_index + 1 :]:
            distance = math.sqrt(
                sum(
                    (
                        (float(left[name]) - float(right[name]))
                        / max(deviations[name], 1.0e-12)
                    )
                    ** 2
                    for name in FEATURE_NAMES
                )
            )
            pairs.append(
                (
                    distance,
                    str(left["profile"]),
                    str(right["profile"]),
                )
            )
    return sorted(pairs)


def main() -> int:
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[1]
    parser.add_argument(
        "--manifest",
        type=Path,
        default=root / "site" / "sound-design-manifest.js",
    )
    parser.add_argument(
        "--roles",
        nargs="+",
        default=["chords", "melody"],
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    manifest = load_manifest(args.manifest)
    records: list[dict[str, object]] = []
    architecture_counts: Counter[tuple[str, str, str]] = Counter()
    for profile in manifest["profiles"]:
        for role in profile["roles"]:
            role_id = role["id"]
            if role_id not in args.roles:
                continue
            parameters = role["parameters"]
            source = parameters["source"]
            filter_name = parameters["filterArchitecture"]
            architecture_counts[(role_id, source, filter_name)] += 1
            candidate = next(
                candidate
                for candidate in role["candidates"]
                if candidate["id"] == "daisy"
            )
            path = args.manifest.parent / candidate["path"]
            rate, samples = load_pcm16(path)
            record: dict[str, object] = {
                "profile": profile["id"],
                "style": profile["styleId"],
                "role": role_id,
                "source": source,
                "filter": filter_name,
            }
            record.update(timbre_metrics(rate, samples))
            records.append(record)
    if args.json:
        print(json.dumps({"records": records}, indent=2))
        return 0

    print(f"Measured Daisy stems: {len(records)}")
    for role_id in args.roles:
        role_records = [
            record for record in records if record["role"] == role_id
        ]
        architectures = [
            (source, filter_name, count)
            for (role, source, filter_name), count
            in architecture_counts.items()
            if role == role_id
        ]
        print(
            f"\n{role_id}: {len(architectures)} source/filter architectures"
        )
        for source, filter_name, count in sorted(architectures):
            print(f"  {source:<24} {filter_name:<25} profiles={count}")
        centroids = [float(record["centroid_hz"]) for record in role_records]
        rolloffs = [float(record["rolloff_85_hz"]) for record in role_records]
        flatness = [float(record["flatness"]) for record in role_records]
        print(
            "  measured ranges:"
            f" centroid={min(centroids):.0f}-{max(centroids):.0f} Hz,"
            f" rolloff85={min(rolloffs):.0f}-{max(rolloffs):.0f} Hz,"
            f" flatness={min(flatness):.4f}-{max(flatness):.4f}"
        )
        print("  per-profile raw measurements:")
        for record in role_records:
            print(
                f"    {record['profile']:<28}"
                f" centroid={record['centroid_hz']:>5.0f}"
                f" rolloff={record['rolloff_85_hz']:>5.0f}"
                f" flat={record['flatness']:.4f}"
                f" bands={record['low_ratio']:.2f}/"
                f"{record['body_ratio']:.2f}/"
                f"{record['presence_ratio']:.2f}/"
                f"{record['air_ratio']:.2f}"
                f" zcr={record['zcr']:.3f}"
                f" diff={record['difference_ratio']:.3f}"
                f" crest={record['crest_db']:.1f}dB"
            )
        print(
            "  nearest standardized feature-vector pairs"
            " (diagnostic only):"
        )
        for distance, left, right in standardized_neighbours(role_records)[:5]:
            print(f"    {distance:.3f}  {left} / {right}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
