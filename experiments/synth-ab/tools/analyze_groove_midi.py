#!/usr/bin/env python3
"""Measure per-style drum velocity behaviour in Google's Groove MIDI Dataset.

The parser intentionally uses only Python's standard library. The resulting
JSON is research evidence, not a runtime dependency or a learned model.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import statistics
import struct
import zipfile
from collections import defaultdict
from pathlib import Path
from typing import Iterable


NOTE_GROUPS = {
    36: "kick",
    38: "snare",
    40: "snare",
    37: "cross-stick",
    48: "tom",
    50: "tom",
    45: "tom",
    47: "tom",
    43: "tom",
    58: "tom",
    46: "open-hat",
    26: "open-hat",
    42: "closed-hat",
    22: "closed-hat",
    44: "closed-hat",
    49: "crash",
    55: "crash",
    57: "crash",
    52: "crash",
    51: "ride",
    59: "ride",
    53: "ride",
}


def read_vlq(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated variable-length MIDI quantity")
        byte = data[offset]
        offset += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, offset


def parse_midi(data: bytes) -> tuple[int, list[tuple[int, int, int]]]:
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File")
    header_size = struct.unpack(">I", data[4:8])[0]
    _, track_count, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise ValueError("SMPTE MIDI division is not supported")
    offset = 8 + header_size
    notes: list[tuple[int, int, int]] = []
    for _ in range(track_count):
        if data[offset : offset + 4] != b"MTrk":
            raise ValueError("missing MIDI track chunk")
        length = struct.unpack(">I", data[offset + 4 : offset + 8])[0]
        track = data[offset + 8 : offset + 8 + length]
        offset += 8 + length
        cursor = 0
        tick = 0
        running_status: int | None = None
        while cursor < len(track):
            delta, cursor = read_vlq(track, cursor)
            tick += delta
            status = track[cursor]
            if status < 0x80:
                if running_status is None:
                    raise ValueError("running status without prior channel event")
                status = running_status
            else:
                cursor += 1
                if status < 0xF0:
                    running_status = status
            if status == 0xFF:
                if cursor >= len(track):
                    raise ValueError("truncated meta event")
                cursor += 1
                size, cursor = read_vlq(track, cursor)
                cursor += size
                continue
            if status in (0xF0, 0xF7):
                size, cursor = read_vlq(track, cursor)
                cursor += size
                running_status = None
                continue
            event_type = status & 0xF0
            if event_type in (0xC0, 0xD0):
                cursor += 1
                continue
            if cursor + 2 > len(track):
                raise ValueError("truncated MIDI channel event")
            data1 = track[cursor]
            data2 = track[cursor + 1]
            cursor += 2
            if event_type == 0x90 and data2:
                notes.append((tick, data1, data2))
    return division, notes


def percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    position = fraction * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    blend = position - lower
    return (
        sorted_values[lower] * (1.0 - blend)
        + sorted_values[upper] * blend
    )


def summarize(values: Iterable[int]) -> dict[str, float | int]:
    ordered = sorted(values)
    if not ordered:
        return {"count": 0}
    return {
        "count": len(ordered),
        "minimum": ordered[0],
        "p05": round(percentile(ordered, 0.05), 2),
        "p20": round(percentile(ordered, 0.20), 2),
        "p25": round(percentile(ordered, 0.25), 2),
        "median": round(percentile(ordered, 0.50), 2),
        "p75": round(percentile(ordered, 0.75), 2),
        "p85": round(percentile(ordered, 0.85), 2),
        "p95": round(percentile(ordered, 0.95), 2),
        "maximum": ordered[-1],
        "mean": round(statistics.fmean(ordered), 2),
        "standardDeviation": round(
            statistics.pstdev(ordered) if len(ordered) > 1 else 0.0,
            2,
        ),
    }


def analyze(archive: Path) -> dict:
    velocities: dict[str, dict[str, list[int]]] = defaultdict(
        lambda: defaultdict(list)
    )
    repeat_deltas: dict[str, dict[str, list[int]]] = defaultdict(
        lambda: defaultdict(list)
    )
    file_counts: dict[str, int] = defaultdict(int)
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        info_name = next(name for name in names if name.endswith("/info.csv"))
        rows = csv.DictReader(
            io.TextIOWrapper(bundle.open(info_name), encoding="utf-8")
        )
        root = info_name[: -len("info.csv")]
        for row in rows:
            style = row["style"].split("/", 1)[0].strip().lower()
            midi_name = root + row["midi_filename"].replace("\\", "/")
            division, notes = parse_midi(bundle.read(midi_name))
            file_counts[style] += 1
            last_velocity: dict[str, int] = {}
            last_tick: dict[str, int] = {}
            for tick, note, velocity in notes:
                group = NOTE_GROUPS.get(note)
                if group is None:
                    continue
                velocities[style][group].append(velocity)
                # Limit this statistic to locally consecutive same-piece hits.
                # A quarter note is deliberately broad enough for ride/hat
                # streams but excludes most unrelated later phrase events.
                if (
                    group in last_velocity
                    and 0 < tick - last_tick[group] <= division
                ):
                    repeat_deltas[style][group].append(
                        abs(velocity - last_velocity[group])
                    )
                last_velocity[group] = velocity
                last_tick[group] = tick

    styles = {}
    for style in sorted(velocities):
        pieces = {}
        for piece in sorted(velocities[style]):
            piece_summary = summarize(velocities[style][piece])
            delta_summary = summarize(repeat_deltas[style][piece])
            pieces[piece] = {
                "velocity": piece_summary,
                "absoluteConsecutiveVelocityDelta": delta_summary,
                "suggestedSemanticBands": {
                    "ghost": [
                        round(piece_summary["p05"]),
                        round(piece_summary["p25"]),
                    ],
                    "normal": [
                        round(piece_summary["p20"]),
                        round(piece_summary["p85"]),
                    ],
                    "accent": [
                        round(piece_summary["p75"]),
                        round(piece_summary["p95"]),
                    ],
                },
            }
        styles[style] = {
            "files": file_counts[style],
            "pieces": pieces,
        }
    return {
        "schema": "jam2-groove-midi-velocity-research",
        "source": "Google Magenta Groove MIDI Dataset v1.0.0 MIDI-only",
        "sourceUrl": "https://magenta.withgoogle.com/datasets/groove",
        "sourceLicense": "CC BY 4.0",
        "archiveSha256": (
            "651cbc524ffb891be1a3e46d89dc82a1cecb09a57c748c7b45b844c4841dcc1e"
        ),
        "method": {
            "semanticBands": {
                "ghost": "p05-p25",
                "normal": "p20-p85",
                "accent": "p75-p95",
            },
            "note": (
                "Bands intentionally overlap: Jam2 semantic articulation chooses "
                "the band, then deterministic within-band variation supplies "
                "natural differences. They are evidence, not inferred labels."
            ),
        },
        "styles": styles,
    }


def print_summary(result: dict) -> None:
    print("| Style | Piece | Hits | p05 | p25 | median | p75 | p95 | repeat delta median |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for style, style_data in result["styles"].items():
        for piece in ("kick", "snare", "closed-hat", "ride"):
            if piece not in style_data["pieces"]:
                continue
            item = style_data["pieces"][piece]
            velocity = item["velocity"]
            repeat = item["absoluteConsecutiveVelocityDelta"]
            print(
                f"| {style} | {piece} | {velocity['count']} | "
                f"{velocity['p05']} | {velocity['p25']} | "
                f"{velocity['median']} | {velocity['p75']} | "
                f"{velocity['p95']} | {repeat.get('median', 0)} |"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.archive)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2) + "\n",
            encoding="utf-8",
        )
    print_summary(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
