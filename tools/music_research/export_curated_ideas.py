#!/usr/bin/env python3
"""Publish two 32-bar drum grooves and compact four-bar previews per profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import wave
from collections import defaultdict
from pathlib import Path


STYLE_NAMES = {
    "blues": "Blues",
    "bossa-nova": "Bossa Nova",
    "country": "Country",
    "electronic": "Electronic",
    "funk": "Funk",
    "hiphop-trap": "Hip-Hop / Trap",
    "jazz": "Jazz",
    "jpop-anisong": "J-Pop / Anisong",
    "metal-experimental": "Modern Metal",
    "modal-jam": "Modal Jam",
    "pop": "Pop",
    "reggae": "Reggae",
    "rnb-soul": "R&B / Soul",
    "rock": "Rock",
}

# Preview playback runs at unity and follows only the application's master
# output. Bake in the same default backing-track trim and prepared-mix gain so
# audition loudness represents a newly generated track without consulting its
# independently adjustable Track fader at runtime.
DEFAULT_BACKING_TRACK_GAIN_DB = -3.0
PREPARED_MIX_MASTER_GAIN = 0.72
DEFAULT_BACKING_TRACK_GAIN = math.pow(
    10.0, DEFAULT_BACKING_TRACK_GAIN_DB / 20.0
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("preview_dir", type=Path)
    return parser.parse_args()


def four_bar_frames(recipe: dict, sample_rate: int) -> int:
    bpm = int(recipe["bpm"])
    numerator = int(recipe["time"]["numerator"])
    pulse_units = max(1, int(recipe["time"].get("tempo_pulse_units", 1)))
    return round(sample_rate * 60.0 / bpm / pulse_units * numerator * 4)


def crop_preview(
    source: Path,
    destination: Path,
    frames: int,
    preview_rate: int = 24000,
) -> None:
    with wave.open(str(source), "rb") as reader:
        if reader.getnchannels() != 1 or reader.getsampwidth() != 2:
            raise ValueError(f"{source} must be mono PCM16")
        source_rate = reader.getframerate()
        count = min(frames, reader.getnframes())
        raw = reader.readframes(count)
    samples = list(struct.unpack(f"<{count}h", raw))
    if source_rate != preview_rate:
        output_count = round(count * preview_rate / source_rate)
        if source_rate % preview_rate == 0:
            samples = samples[::source_rate // preview_rate][:output_count]
        else:
            resampled = []
            for output_index in range(output_count):
                position = output_index * source_rate / preview_rate
                left = min(int(position), count - 1)
                right = min(left + 1, count - 1)
                fraction = position - left
                resampled.append(round(
                    samples[left] * (1.0 - fraction) + samples[right] * fraction))
            samples = resampled
        count = len(samples)
    fade = min(round(preview_rate * 0.02), count // 2)
    # Match PreparedMixRenderer exactly for a single unity-gain drum lane:
    # convert PCM16 to float, apply the master pre-gain and tanh limiter, then
    # apply the default backing-track fader. Preview playback itself is unity.
    samples = [round(
        32767.0
        * math.tanh(PREPARED_MIX_MASTER_GAIN * sample / 32768.0)
        * DEFAULT_BACKING_TRACK_GAIN
    ) for sample in samples]
    for index in range(fade):
        gain = index / max(1, fade)
        samples[index] = round(samples[index] * gain)
        samples[count - 1 - index] = round(samples[count - 1 - index] * gain)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(destination), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(preview_rate)
        writer.writeframes(struct.pack(f"<{count}h", *samples))


def main() -> int:
    args = parse_args()
    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    candidates = [
        sample for sample in corpus["samples"]
        if sample.get("audio_drums")
        and sample.get("requested_complexity") == 4
        and sample.get("requested_bars") == 32
    ]
    selected: list[dict] = []
    counts: defaultdict[str, int] = defaultdict(int)
    for sample in candidates:
        profile = sample["profile_id"]
        if counts[profile] >= 2:
            continue
        counts[profile] += 1
        selected.append(sample)
    if len(counts) != 27 or any(count != 2 for count in counts.values()):
        raise ValueError(
            f"expected two ideas for each of 27 profiles, got {dict(counts)}")

    args.preview_dir.mkdir(parents=True, exist_ok=True)
    for previous in args.preview_dir.glob("*.wav"):
        previous.unlink()
    ideas = []
    corpus_root = args.corpus.parent
    for sample in selected:
        recipe = sample["recipe"]
        meter = recipe["time"]
        fingerprints = recipe["fingerprints"]
        take = int(sample["sample_index"]) + 1
        idea_id = f'{sample["profile_id"]}__groove-{take}'
        preview_name = f"{idea_id}.wav"
        preview_path = args.preview_dir / preview_name
        source_path = corpus_root / sample["audio_drums"]
        with wave.open(str(source_path), "rb") as source:
            sample_rate = source.getframerate()
        crop_preview(source_path, preview_path, four_bar_frames(recipe, sample_rate))
        preview_hash = hashlib.sha256(preview_path.read_bytes()).hexdigest()
        ideas.append({
            "id": idea_id,
            "name": f'{recipe["groove"]["name"]} — Take {take}',
            "style_id": sample["style_id"],
            "style_name": STYLE_NAMES[sample["style_id"]],
            "profile_id": sample["profile_id"],
            "profile_name": sample["profile_name"],
            "form_id": sample["form_id"],
            "form_name": sample["form_name"],
            "seed": str(sample["seed"]),
            "generator_version": recipe["generator_version"],
            "complexity": sample["requested_complexity"],
            "bpm": recipe["bpm"],
            "meter_id": meter["meter_id"],
            "meter_numerator": meter["numerator"],
            "meter_denominator": meter["denominator"],
            "bars": recipe["bars"],
            "tonic": recipe["tonic"],
            "mode": recipe["mode"],
            "chord_fingerprint": fingerprints["chord"],
            "beat_fingerprint": fingerprints["beat"],
            "preview_bars": 4,
            "preview_resource": f":/jam2/ideas/previews/{preview_name}",
            "preview_sha256": preview_hash,
        })

    args.catalog.parent.mkdir(parents=True, exist_ok=True)
    args.catalog.write_text(
        json.dumps({"version": 1, "ideas": ideas}, indent=2) + "\n",
        encoding="utf-8",
    )
    total_bytes = sum(path.stat().st_size for path in args.preview_dir.glob("*.wav"))
    print(
        f"published {len(ideas)} ideas across {len(counts)} profiles; "
        f"preview bytes={total_bytes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
