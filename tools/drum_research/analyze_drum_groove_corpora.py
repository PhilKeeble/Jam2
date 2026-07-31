#!/usr/bin/env python3
"""Extract inspectable timing/pocket evidence from Drum Groove Corpora CSV."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import Counter, defaultdict
from dataclasses import dataclass, field
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
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    return {
        "count": len(values),
        "minimum": round(min(values), 6),
        "p05": round(percentile(values, 0.05), 6),
        "p20": round(percentile(values, 0.20), 6),
        "p25": round(percentile(values, 0.25), 6),
        "median": round(percentile(values, 0.50), 6),
        "p75": round(percentile(values, 0.75), 6),
        "p85": round(percentile(values, 0.85), 6),
        "p95": round(percentile(values, 0.95), 6),
        "maximum": round(max(values), 6),
        "mean": round(mean, 6),
        "standardDeviation": round(math.sqrt(variance), 6),
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def similarity(left: set[tuple[str, int]], right: set[tuple[str, int]]) -> float:
    union = left | right
    return len(left & right) / len(union) if union else 1.0


@dataclass
class Track:
    corpus: str
    drummer: str
    tempo: float
    duration: float
    hits: list[tuple[str, float, float]] = field(default_factory=list)


@dataclass
class Aggregate:
    tracks: int = 0
    drummers: set[str] = field(default_factory=set)
    tempos: list[float] = field(default_factory=list)
    durations: list[float] = field(default_factory=list)
    bars: list[float] = field(default_factory=list)
    hits_per_bar: list[float] = field(default_factory=list)
    instrument_counts: Counter[str] = field(default_factory=Counter)
    timing_ms: list[float] = field(default_factory=list)
    timing_by_instrument: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    timing_by_slot: dict[int, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    limb_offsets: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    adjacent_similarity: list[float] = field(default_factory=list)
    period2_similarity: list[float] = field(default_factory=list)
    period4_similarity: list[float] = field(default_factory=list)

    def add(self, track: Track) -> None:
        self.tracks += 1
        self.drummers.add(track.drummer)
        self.tempos.append(track.tempo)
        self.durations.append(track.duration)
        metric_times = [hit[1] for hit in track.hits]
        span = max(metric_times) - min(metric_times) if metric_times else 0.0
        bars = max(1.0, span / 4.0)
        self.bars.append(bars)
        self.hits_per_bar.append(len(track.hits) / bars)

        positions: dict[float, dict[str, list[float]]] = defaultdict(
            lambda: defaultdict(list)
        )
        bars_by_index: dict[int, set[tuple[str, int]]] = defaultdict(set)
        for instrument, metric_time, timing_ms in track.hits:
            self.instrument_counts[instrument] += 1
            self.timing_ms.append(timing_ms)
            self.timing_by_instrument[instrument].append(timing_ms)
            slot = int(round((metric_time % 4.0) * 4.0)) % 16
            self.timing_by_slot[slot].append(timing_ms)
            metric_key = round(metric_time * 4.0) / 4.0
            positions[metric_key][instrument].append(timing_ms)
            bars_by_index[int(math.floor(metric_time / 4.0))].add(
                (instrument, slot)
            )

        for instruments in positions.values():
            means = {
                instrument: sum(values) / len(values)
                for instrument, values in instruments.items()
            }
            for left, right in (("SD", "BD"), ("HH", "BD"), ("SD", "HH")):
                if left in means and right in means:
                    self.limb_offsets[f"{left}-minus-{right}"].append(
                        means[left] - means[right]
                    )

        ordered_bars = [
            bars_by_index[index]
            for index in sorted(bars_by_index)
        ]
        self.adjacent_similarity.extend(
            similarity(left, right)
            for left, right in zip(ordered_bars, ordered_bars[1:])
        )
        self.period2_similarity.extend(
            similarity(ordered_bars[index], ordered_bars[index + 2])
            for index in range(max(0, len(ordered_bars) - 2))
        )
        self.period4_similarity.extend(
            similarity(ordered_bars[index], ordered_bars[index + 4])
            for index in range(max(0, len(ordered_bars) - 4))
        )

    def result(self) -> dict[str, object]:
        return {
            "tracks": self.tracks,
            "drummers": len(self.drummers),
            "tempoBpm": summarize(self.tempos),
            "trackDurationSeconds": summarize(self.durations),
            "estimatedBars": summarize(self.bars),
            "hitsPerBar": summarize(self.hits_per_bar),
            "instrumentCounts": dict(sorted(self.instrument_counts.items())),
            "microtimingMs": summarize(self.timing_ms),
            "microtimingMsByInstrument": {
                name: summarize(values)
                for name, values in sorted(self.timing_by_instrument.items())
            },
            "microtimingMsBySixteenthSlot": {
                str(slot): summarize(values)
                for slot, values in sorted(self.timing_by_slot.items())
            },
            "coincidentLimbOffsetMs": {
                name: summarize(values)
                for name, values in sorted(self.limb_offsets.items())
            },
            "sequence": {
                "adjacentPresenceSimilarity": summarize(
                    self.adjacent_similarity
                ),
                "period2PresenceSimilarity": summarize(
                    self.period2_similarity
                ),
                "period4PresenceSimilarity": summarize(
                    self.period4_similarity
                ),
            },
        }


def analyze(path: Path) -> dict[str, object]:
    tracks: dict[str, Track] = {}
    rows = 0
    with path.open("r", encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            rows += 1
            track_id = f"{row['Corpus']}|{row['Track']}"
            track = tracks.get(track_id)
            if track is None:
                track = Track(
                    corpus=row["Corpus"],
                    drummer=row["Drummer"],
                    tempo=float(row["Tempo"]),
                    duration=float(row["TrackDuration"]),
                )
                tracks[track_id] = track
            track.hits.append(
                (
                    row["Instrument"],
                    float(row["MetricTime"]),
                    1000.0 * float(row["MicrotimingSeconds"]),
                )
            )

    overall = Aggregate()
    by_corpus: dict[str, Aggregate] = defaultdict(Aggregate)
    by_drummer: dict[str, Aggregate] = defaultdict(Aggregate)
    for track in tracks.values():
        overall.add(track)
        by_corpus[track.corpus].add(track)
        by_drummer[f"{track.corpus}|{track.drummer}"].add(track)

    return {
        "schema": "jam2-drum-groove-corpora-evidence-v1",
        "source": {
            "path": str(path),
            "sha256": sha256(path),
            "rows": rows,
            "tracks": len(tracks),
            "license": "CC 4.0 per the Drum Groove Corpora data report",
        },
        "method": {
            "metricalGrid": "sixteenth positions in four-beat bars",
            "microtiming": (
                "Published onset minus per-track regression metronome, "
                "reported in milliseconds"
            ),
            "sequencePresence": (
                "Jaccard similarity over BD/SD/HH sixteenth-position sets"
            ),
            "limitations": [
                "Only bass drum, snare drum, and hi-hat are represented.",
                "No velocity, articulation, tom, cymbal, fill, or genre field is available.",
                "Loop and Lucerne include commercial-derived timing metadata; no audio is used.",
                "Magenta overlaps GMD, so it is not treated as independent evidence.",
            ],
        },
        "overall": overall.result(),
        "byCorpus": {
            name: aggregate.result()
            for name, aggregate in sorted(by_corpus.items())
        },
        "byDrummer": {
            name: aggregate.result()
            for name, aggregate in sorted(by_drummer.items())
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.csv)
    encoded = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
