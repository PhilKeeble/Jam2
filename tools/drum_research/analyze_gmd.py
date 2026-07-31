#!/usr/bin/env python3
"""Create reproducible performance evidence from Google Groove MIDI Dataset.

Only the Python standard library is used. The output is research evidence for
Jam2's deterministic authored drum rules; this is not a runtime model.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import itertools
import json
import math
import statistics
import struct
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Iterator, Protocol


@dataclass(frozen=True)
class NoteIdentity:
    piece: str
    articulation: str
    tom_rank: int | None = None


NOTE_IDENTITIES = {
    36: NoteIdentity("kick", "head"),
    38: NoteIdentity("snare", "head"),
    40: NoteIdentity("snare", "rim"),
    37: NoteIdentity("cross-stick", "cross-stick"),
    48: NoteIdentity("high-tom", "head", 2),
    50: NoteIdentity("high-tom", "rim", 2),
    45: NoteIdentity("mid-tom", "head", 1),
    47: NoteIdentity("mid-tom", "rim", 1),
    43: NoteIdentity("floor-tom", "head", 0),
    58: NoteIdentity("floor-tom", "rim", 0),
    46: NoteIdentity("open-hat", "bow"),
    26: NoteIdentity("open-hat", "edge"),
    42: NoteIdentity("closed-hat", "bow"),
    22: NoteIdentity("closed-hat", "edge"),
    44: NoteIdentity("closed-hat", "pedal"),
    49: NoteIdentity("crash", "bow-1"),
    55: NoteIdentity("crash", "edge-1"),
    57: NoteIdentity("crash", "bow-2"),
    52: NoteIdentity("crash", "edge-2"),
    51: NoteIdentity("ride", "bow"),
    59: NoteIdentity("ride", "edge"),
    53: NoteIdentity("ride", "bell"),
}

PIECE_ORDER = (
    "kick",
    "snare",
    "closed-hat",
    "open-hat",
    "high-tom",
    "mid-tom",
    "floor-tom",
    "crash",
    "ride",
    "cross-stick",
)


@dataclass(frozen=True)
class MidiNote:
    tick: int
    pitch: int
    velocity: int


@dataclass(frozen=True)
class ParsedMidi:
    division: int
    notes: tuple[MidiNote, ...]
    final_tick: int
    hi_hat_controls: tuple[tuple[int, int], ...]


class DatasetSource(Protocol):
    def metadata_text(self) -> str: ...

    def midi_bytes(self, relative_name: str) -> bytes: ...

    def identity(self) -> dict[str, object]: ...

    def close(self) -> None: ...


class DirectorySource:
    def __init__(self, path: Path):
        candidates = (path / "info.csv", path / "groove" / "info.csv")
        self.root = next((item.parent for item in candidates if item.is_file()), None)
        if self.root is None:
            raise ValueError(f"no info.csv below {path}")

    def metadata_text(self) -> str:
        return (self.root / "info.csv").read_text(encoding="utf-8-sig")

    def midi_bytes(self, relative_name: str) -> bytes:
        return (self.root / Path(relative_name)).read_bytes()

    def identity(self) -> dict[str, object]:
        metadata = (self.root / "info.csv").read_bytes()
        return {
            "kind": "directory",
            "metadataSha256": hashlib.sha256(metadata).hexdigest(),
            "rootName": self.root.name,
        }

    def close(self) -> None:
        return


class ZipSource:
    def __init__(self, path: Path):
        self.path = path
        self.archive = zipfile.ZipFile(path)
        info_names = [
            name for name in self.archive.namelist() if name.endswith("/info.csv")
        ]
        if not info_names and "info.csv" in self.archive.namelist():
            info_names = ["info.csv"]
        if len(info_names) != 1:
            raise ValueError(f"expected one info.csv in {path}, found {len(info_names)}")
        self.info_name = info_names[0]
        self.root = PurePosixPath(self.info_name).parent

    def metadata_text(self) -> str:
        return self.archive.read(self.info_name).decode("utf-8-sig")

    def midi_bytes(self, relative_name: str) -> bytes:
        name = str(self.root / PurePosixPath(relative_name.replace("\\", "/")))
        return self.archive.read(name)

    def identity(self) -> dict[str, object]:
        digest = hashlib.sha256()
        with self.path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return {
            "kind": "zip",
            "archiveName": self.path.name,
            "archiveSha256": digest.hexdigest(),
        }

    def close(self) -> None:
        self.archive.close()


def open_source(path: Path) -> DatasetSource:
    if path.is_dir():
        return DirectorySource(path)
    if path.is_file() and zipfile.is_zipfile(path):
        return ZipSource(path)
    raise ValueError(f"dataset must be an extracted GMD directory or zip: {path}")


def read_vlq(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for _ in range(4):
        if offset >= len(data):
            raise ValueError("truncated variable-length MIDI quantity")
        byte = data[offset]
        offset += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, offset
    raise ValueError("invalid MIDI variable-length quantity")


def parse_midi(data: bytes) -> ParsedMidi:
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File")
    header_size = struct.unpack(">I", data[4:8])[0]
    if header_size < 6 or 8 + header_size > len(data):
        raise ValueError("invalid MIDI header")
    _, track_count, division = struct.unpack(">HHH", data[8:14])
    if division == 0 or division & 0x8000:
        raise ValueError("SMPTE or zero MIDI division is unsupported")
    offset = 8 + header_size
    notes: list[MidiNote] = []
    hi_hat_controls: list[tuple[int, int]] = []
    final_tick = 0
    for _ in range(track_count):
        if offset + 8 > len(data) or data[offset : offset + 4] != b"MTrk":
            raise ValueError("missing MIDI track chunk")
        length = struct.unpack(">I", data[offset + 4 : offset + 8])[0]
        track_end = offset + 8 + length
        if track_end > len(data):
            raise ValueError("truncated MIDI track")
        track = data[offset + 8 : track_end]
        offset = track_end
        cursor = 0
        tick = 0
        running_status: int | None = None
        while cursor < len(track):
            delta, cursor = read_vlq(track, cursor)
            tick += delta
            final_tick = max(final_tick, tick)
            if cursor >= len(track):
                raise ValueError("truncated MIDI event")
            status = track[cursor]
            if status < 0x80:
                if running_status is None:
                    raise ValueError("running status without channel status")
                status = running_status
            else:
                cursor += 1
                if status < 0xF0:
                    running_status = status
            if status == 0xFF:
                if cursor >= len(track):
                    raise ValueError("truncated MIDI meta event")
                cursor += 1
                size, cursor = read_vlq(track, cursor)
                cursor += size
                if cursor > len(track):
                    raise ValueError("truncated MIDI meta payload")
                running_status = None
                continue
            if status in (0xF0, 0xF7):
                size, cursor = read_vlq(track, cursor)
                cursor += size
                if cursor > len(track):
                    raise ValueError("truncated MIDI system payload")
                running_status = None
                continue
            event_type = status & 0xF0
            channel = status & 0x0F
            data_size = 1 if event_type in (0xC0, 0xD0) else 2
            if cursor + data_size > len(track):
                raise ValueError("truncated MIDI channel event")
            data1 = track[cursor]
            data2 = track[cursor + 1] if data_size == 2 else 0
            cursor += data_size
            if channel != 9:
                continue
            if event_type == 0x90 and data2:
                notes.append(MidiNote(tick, data1, data2))
            elif event_type == 0xB0 and data1 == 4:
                hi_hat_controls.append((tick, data2))
    notes.sort(key=lambda item: (item.tick, item.pitch))
    hi_hat_controls.sort()
    return ParsedMidi(
        division,
        tuple(notes),
        final_tick,
        tuple(hi_hat_controls),
    )


def percentile(ordered: list[float], fraction: float) -> float:
    if not ordered:
        return 0.0
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    blend = position - lower
    return ordered[lower] * (1.0 - blend) + ordered[upper] * blend


def summarize(values: Iterable[float], digits: int = 3) -> dict[str, float | int]:
    ordered = sorted(values)
    if not ordered:
        return {"count": 0}

    def rounded(value: float) -> float:
        return round(value, digits)

    return {
        "count": len(ordered),
        "minimum": rounded(ordered[0]),
        "p05": rounded(percentile(ordered, 0.05)),
        "p20": rounded(percentile(ordered, 0.20)),
        "p25": rounded(percentile(ordered, 0.25)),
        "median": rounded(percentile(ordered, 0.50)),
        "p75": rounded(percentile(ordered, 0.75)),
        "p85": rounded(percentile(ordered, 0.85)),
        "p95": rounded(percentile(ordered, 0.95)),
        "maximum": rounded(ordered[-1]),
        "mean": rounded(statistics.fmean(ordered)),
        "standardDeviation": rounded(
            statistics.pstdev(ordered) if len(ordered) > 1 else 0.0
        ),
    }


def signed_grid_residual(tick: int, division: int, steps_per_quarter: int) -> float:
    step_ticks = division / steps_per_quarter
    nearest = round(tick / step_ticks) * step_ticks
    return (tick - nearest) / division


def select_grid(
    straight_residuals: list[float],
    triplet_residuals: list[float],
) -> tuple[str, int, int]:
    """Select feel using only positions that distinguish the candidate grids.

    A naive mean-error comparison is biased toward the six-step grid because it
    has more candidate positions. Quarter- and eighth-note anchors also fit both
    grids and carry no feel evidence. Count only events for which one grid is
    materially closer, then require a clear triplet majority; ambiguous files
    remain straight and retain both raw residual reports.
    """

    straight_votes = 0
    triplet_votes = 0
    for straight, triplet in zip(straight_residuals, triplet_residuals):
        straight_error = abs(straight)
        triplet_error = abs(triplet)
        if abs(straight_error - triplet_error) < 0.025:
            continue
        if straight_error < triplet_error:
            straight_votes += 1
        else:
            triplet_votes += 1
    selected = (
        "triplet-sixth"
        if triplet_votes >= 3 and triplet_votes > straight_votes * 1.20
        else "straight-sixteenth"
    )
    return selected, straight_votes, triplet_votes


def parse_time_signature(value: str) -> tuple[int, int]:
    try:
        numerator_text, denominator_text = value.strip().split("-", 1)
        numerator = int(numerator_text)
        denominator = int(denominator_text)
    except (ValueError, TypeError) as error:
        raise ValueError(f"invalid time signature {value!r}") from error
    if numerator <= 0 or denominator not in (1, 2, 4, 8, 16):
        raise ValueError(f"unsupported time signature {value!r}")
    return numerator, denominator


def primary_style(value: str) -> str:
    return value.split("/", 1)[0].strip().lower()


def tempo_band(bpm: int) -> str:
    if bpm < 80:
        return "under-80"
    if bpm < 110:
        return "80-109"
    if bpm < 140:
        return "110-139"
    return "140-plus"


def ratio(numerator: int, denominator: int) -> float:
    return round(numerator / denominator, 4) if denominator else 0.0


def signature_similarity(
    left: tuple[tuple[int, str], ...],
    right: tuple[tuple[int, str], ...],
) -> float:
    left_set = set(left)
    right_set = set(right)
    union = left_set | right_set
    return len(left_set & right_set) / len(union) if union else 1.0


def sequence_features(
    midi: ParsedMidi,
    known: list[tuple[MidiNote, NoteIdentity]],
    grid_name: str,
    numerator: int,
    denominator: int,
) -> dict[str, object]:
    """Measure active bar-to-bar development without copying source patterns.

    Signatures retain only quantized instrument presence. Velocity, residual
    timing, and articulation remain in their existing aggregate reports. This
    deliberately makes the sequence comparison answer the same structural
    question as Jam2's corpus audit: whether the pocket is recalled, varied,
    or replaced across the performed span.
    """

    if not known:
        return {
            "activeBars": 0,
            "uniqueBarRatio": 0.0,
            "uniqueCoreBarRatio": 0.0,
            "period2CoreIdenticalRatio": 0.0,
            "period4CoreIdenticalRatio": 0.0,
            "consecutiveCoreIdenticalRatio": 0.0,
            "adjacentCoreSymmetricChanges": [],
            "adjacentCoreSimilarity": [],
            "period2CoreSimilarity": [],
            "period4CoreSimilarity": [],
            "boundaryCoreSymmetricChanges": {"2": [], "4": [], "8": []},
            "boundaryCoreSimilarity": {"2": [], "4": [], "8": []},
            "barHitCounts": [],
            "barMeanVelocities": [],
            "quarterHitSpread": 0.0,
        }

    quarter_beats_per_bar = numerator * 4.0 / denominator
    ticks_per_bar = midi.division * quarter_beats_per_bar
    steps_per_quarter = 6 if grid_name == "triplet-sixth" else 4
    slots_per_bar = max(1, round(quarter_beats_per_bar * steps_per_quarter))
    active_bars = max(
        1,
        int(math.ceil((max(note.tick for note, _ in known) + 1) / ticks_per_bar)),
    )
    bar_hits: list[list[tuple[int, str]]] = [
        [] for _ in range(active_bars)
    ]
    bar_velocities: list[list[int]] = [[] for _ in range(active_bars)]
    for note, identity in known:
        bar = min(active_bars - 1, int(note.tick // ticks_per_bar))
        local_quarter = (note.tick - bar * ticks_per_bar) / midi.division
        slot = max(
            0,
            min(slots_per_bar - 1, round(local_quarter * steps_per_quarter)),
        )
        bar_hits[bar].append((slot, identity.piece))
        bar_velocities[bar].append(note.velocity)

    all_signatures = [tuple(sorted(hits)) for hits in bar_hits]
    core_pieces = {
        "kick",
        "snare",
        "closed-hat",
        "open-hat",
        "ride",
        "cross-stick",
    }
    core_signatures = [
        tuple(sorted(hit for hit in hits if hit[1] in core_pieces))
        for hits in bar_hits
    ]
    adjacent_changes = [
        len(set(core_signatures[index - 1]).symmetric_difference(
            core_signatures[index]
        ))
        for index in range(1, active_bars)
    ]
    adjacent_similarity = [
        signature_similarity(
            core_signatures[index - 1], core_signatures[index]
        )
        for index in range(1, active_bars)
    ]
    period2_similarity = [
        signature_similarity(
            core_signatures[index - 2], core_signatures[index]
        )
        for index in range(2, active_bars)
    ]
    period4_similarity = [
        signature_similarity(
            core_signatures[index - 4], core_signatures[index]
        )
        for index in range(4, active_bars)
    ]
    boundary_changes = {
        str(width): [
            adjacent_changes[bar - 1]
            for bar in range(width, active_bars, width)
        ]
        for width in (2, 4, 8)
    }
    boundary_similarity = {
        str(width): [
            adjacent_similarity[bar - 1]
            for bar in range(width, active_bars, width)
        ]
        for width in (2, 4, 8)
    }
    quarter = max(1, active_bars // 4)
    quarter_hits = [
        sum(len(bar_hits[bar]) for bar in range(first, min(
            active_bars, first + quarter
        )))
        for first in range(0, active_bars, quarter)
    ]
    nonzero_quarters = [value for value in quarter_hits if value > 0]
    quarter_spread = (
        (max(nonzero_quarters) - min(nonzero_quarters))
        / max(nonzero_quarters)
        if len(nonzero_quarters) > 1
        else 0.0
    )
    return {
        "activeBars": active_bars,
        "uniqueBarRatio": ratio(len(set(all_signatures)), active_bars),
        "uniqueCoreBarRatio": ratio(
            len(set(core_signatures)), active_bars
        ),
        "period2CoreIdenticalRatio": ratio(
            sum(
                core_signatures[index] == core_signatures[index - 2]
                for index in range(2, active_bars)
            ),
            max(0, active_bars - 2),
        ),
        "period4CoreIdenticalRatio": ratio(
            sum(
                core_signatures[index] == core_signatures[index - 4]
                for index in range(4, active_bars)
            ),
            max(0, active_bars - 4),
        ),
        "consecutiveCoreIdenticalRatio": ratio(
            sum(
                core_signatures[index] == core_signatures[index - 1]
                for index in range(1, active_bars)
            ),
            max(0, active_bars - 1),
        ),
        "adjacentCoreSymmetricChanges": adjacent_changes,
        "adjacentCoreSimilarity": adjacent_similarity,
        "period2CoreSimilarity": period2_similarity,
        "period4CoreSimilarity": period4_similarity,
        "boundaryCoreSymmetricChanges": boundary_changes,
        "boundaryCoreSimilarity": boundary_similarity,
        "barHitCounts": [len(hits) for hits in bar_hits],
        "barMeanVelocities": [
            sum(values) / len(values) if values else 0.0
            for values in bar_velocities
        ],
        "quarterHitSpread": quarter_spread,
    }


class Aggregate:
    def __init__(self) -> None:
        self.files = 0
        self.beats = 0
        self.fills = 0
        self.bars: list[float] = []
        self.hits_per_bar: list[float] = []
        self.velocity: dict[str, list[float]] = defaultdict(list)
        self.timing_ms: dict[str, list[float]] = defaultdict(list)
        self.straight_timing_ms: dict[str, list[float]] = defaultdict(list)
        self.triplet_timing_ms: dict[str, list[float]] = defaultdict(list)
        self.repeat_velocity_delta: dict[str, list[float]] = defaultdict(list)
        self.repeat_timing_delta_ms: dict[str, list[float]] = defaultdict(list)
        self.articulations: Counter[str] = Counter()
        self.metric_slots: dict[str, Counter[int]] = defaultdict(Counter)
        self.cooccurrence: Counter[str] = Counter()
        self.hat_transitions: Counter[str] = Counter()
        self.tom_directions: Counter[str] = Counter()
        self.grid_choices: Counter[str] = Counter()
        self.grid_evidence_votes: Counter[str] = Counter()
        self.unknown_notes: Counter[int] = Counter()
        self.active_bars: list[float] = []
        self.unique_bar_ratio: list[float] = []
        self.unique_core_bar_ratio: list[float] = []
        self.period2_core_ratio: list[float] = []
        self.period4_core_ratio: list[float] = []
        self.consecutive_core_ratio: list[float] = []
        self.adjacent_core_changes: list[float] = []
        self.adjacent_core_similarity: list[float] = []
        self.period2_core_similarity: list[float] = []
        self.period4_core_similarity: list[float] = []
        self.boundary_core_changes: dict[str, list[float]] = defaultdict(list)
        self.boundary_core_similarity: dict[str, list[float]] = defaultdict(list)
        self.bar_hit_counts: list[float] = []
        self.bar_mean_velocities: list[float] = []
        self.quarter_hit_spread: list[float] = []

    def add_file(self, row: dict[str, str], midi: ParsedMidi) -> None:
        bpm = int(row["bpm"])
        numerator, denominator = parse_time_signature(row["time_signature"])
        quarter_beats_per_bar = numerator * 4.0 / denominator
        bars = (
            midi.final_tick / midi.division / quarter_beats_per_bar
            if midi.final_tick > 0
            else 0.0
        )
        known = [
            (note, NOTE_IDENTITIES[note.pitch])
            for note in midi.notes
            if note.pitch in NOTE_IDENTITIES
        ]
        for note in midi.notes:
            if note.pitch not in NOTE_IDENTITIES:
                self.unknown_notes[note.pitch] += 1
        straight_residuals = [
            signed_grid_residual(note.tick, midi.division, 4) for note, _ in known
        ]
        triplet_residuals = [
            signed_grid_residual(note.tick, midi.division, 6) for note, _ in known
        ]
        grid_name, straight_votes, triplet_votes = select_grid(
            straight_residuals, triplet_residuals
        )
        residuals = triplet_residuals if grid_name == "triplet-sixth" else straight_residuals
        self.grid_choices[grid_name] += 1
        self.grid_evidence_votes["straight"] += straight_votes
        self.grid_evidence_votes["triplet"] += triplet_votes
        sequence = sequence_features(
            midi, known, grid_name, numerator, denominator
        )
        self.active_bars.append(float(sequence["activeBars"]))
        self.unique_bar_ratio.append(float(sequence["uniqueBarRatio"]))
        self.unique_core_bar_ratio.append(
            float(sequence["uniqueCoreBarRatio"])
        )
        self.period2_core_ratio.append(
            float(sequence["period2CoreIdenticalRatio"])
        )
        self.period4_core_ratio.append(
            float(sequence["period4CoreIdenticalRatio"])
        )
        self.consecutive_core_ratio.append(
            float(sequence["consecutiveCoreIdenticalRatio"])
        )
        self.adjacent_core_changes.extend(
            float(value)
            for value in sequence["adjacentCoreSymmetricChanges"]
        )
        self.adjacent_core_similarity.extend(
            float(value)
            for value in sequence["adjacentCoreSimilarity"]
        )
        self.period2_core_similarity.extend(
            float(value)
            for value in sequence["period2CoreSimilarity"]
        )
        self.period4_core_similarity.extend(
            float(value)
            for value in sequence["period4CoreSimilarity"]
        )
        for width, values in sequence[
            "boundaryCoreSymmetricChanges"
        ].items():
            self.boundary_core_changes[width].extend(
                float(value) for value in values
            )
        for width, values in sequence[
            "boundaryCoreSimilarity"
        ].items():
            self.boundary_core_similarity[width].extend(
                float(value) for value in values
            )
        self.bar_hit_counts.extend(
            float(value) for value in sequence["barHitCounts"]
        )
        self.bar_mean_velocities.extend(
            float(value) for value in sequence["barMeanVelocities"]
        )
        self.quarter_hit_spread.append(
            float(sequence["quarterHitSpread"])
        )
        self.files += 1
        if row["beat_type"].strip().lower() == "fill":
            self.fills += 1
        else:
            self.beats += 1
        self.bars.append(bars)
        if bars > 0:
            self.hits_per_bar.append(len(known) / bars)

        last_by_piece: dict[str, tuple[MidiNote, float]] = {}
        slots: dict[int, set[str]] = defaultdict(set)
        tom_sequence: list[tuple[int, int]] = []
        hat_sequence: list[str] = []
        steps_per_quarter = 6 if grid_name == "triplet-sixth" else 4
        slots_per_bar = max(1, round(quarter_beats_per_bar * steps_per_quarter))
        for event_index, ((note, identity), residual_beats) in enumerate(
            zip(known, residuals)
        ):
            piece = identity.piece
            timing_ms = residual_beats * 60_000.0 / bpm
            self.velocity[piece].append(note.velocity)
            self.timing_ms[piece].append(timing_ms)
            self.straight_timing_ms[piece].append(
                straight_residuals[event_index] * 60_000.0 / bpm
            )
            self.triplet_timing_ms[piece].append(
                triplet_residuals[event_index] * 60_000.0 / bpm
            )
            self.articulations[f"{piece}:{identity.articulation}"] += 1
            slot = round(note.tick / midi.division * steps_per_quarter)
            slot_in_bar = slot % slots_per_bar
            self.metric_slots[piece][slot_in_bar] += 1
            slots[slot].add(piece)
            previous = last_by_piece.get(piece)
            if previous is not None:
                previous_note, previous_timing_ms = previous
                interval_beats = (note.tick - previous_note.tick) / midi.division
                if 0 < interval_beats <= 1.0:
                    self.repeat_velocity_delta[piece].append(
                        abs(note.velocity - previous_note.velocity)
                    )
                    self.repeat_timing_delta_ms[piece].append(
                        abs(timing_ms - previous_timing_ms)
                    )
            last_by_piece[piece] = (note, timing_ms)
            if identity.tom_rank is not None:
                tom_sequence.append((note.tick, identity.tom_rank))
            if piece in ("closed-hat", "open-hat"):
                hat_sequence.append(f"{piece}:{identity.articulation}")

        for pieces in slots.values():
            for left, right in itertools.combinations(sorted(pieces), 2):
                self.cooccurrence[f"{left}+{right}"] += 1
        for left, right in itertools.pairwise(hat_sequence):
            self.hat_transitions[f"{left}>{right}"] += 1
        for (_, left), (_, right) in itertools.pairwise(tom_sequence):
            direction = "descending" if right < left else "ascending" if right > left else "same"
            self.tom_directions[direction] += 1

    def json(self) -> dict[str, object]:
        pieces: dict[str, object] = {}
        for piece in PIECE_ORDER:
            if piece not in self.velocity:
                continue
            velocity = summarize(self.velocity[piece], digits=2)
            pieces[piece] = {
                "velocity": velocity,
                "suggestedOverlappingSemanticBands": {
                    "ghost": [round(velocity["p05"]), round(velocity["p25"])],
                    "normal": [round(velocity["p20"]), round(velocity["p85"])],
                    "accent": [round(velocity["p75"]), round(velocity["p95"])],
                },
                "selectedGridTimingOffsetMs": summarize(
                    self.timing_ms[piece], digits=3
                ),
                "straightSixteenthTimingOffsetMs": summarize(
                    self.straight_timing_ms[piece], digits=3
                ),
                "tripletSixthTimingOffsetMs": summarize(
                    self.triplet_timing_ms[piece], digits=3
                ),
                "absoluteConsecutiveVelocityDelta": summarize(
                    self.repeat_velocity_delta[piece], digits=2
                ),
                "absoluteConsecutiveTimingDeltaMs": summarize(
                    self.repeat_timing_delta_ms[piece], digits=3
                ),
                "metricalSlotCounts": dict(
                    sorted(self.metric_slots[piece].items())
                ),
            }
        return {
            "files": self.files,
            "beats": self.beats,
            "fills": self.fills,
            "bars": summarize(self.bars, digits=3),
            "hitsPerBar": summarize(self.hits_per_bar, digits=3),
            "gridChoices": dict(self.grid_choices),
            "gridEvidenceVotes": dict(self.grid_evidence_votes),
            "pieces": pieces,
            "articulationCounts": dict(self.articulations.most_common()),
            "cooccurrenceCounts": dict(self.cooccurrence.most_common()),
            "hatTransitionCounts": dict(self.hat_transitions.most_common()),
            "tomDirectionCounts": dict(self.tom_directions.most_common()),
            "sequence": {
                "activeBars": summarize(self.active_bars, digits=3),
                "uniqueBarRatio": summarize(
                    self.unique_bar_ratio, digits=4
                ),
                "uniqueCoreBarRatio": summarize(
                    self.unique_core_bar_ratio, digits=4
                ),
                "period2CoreIdenticalRatio": summarize(
                    self.period2_core_ratio, digits=4
                ),
                "period4CoreIdenticalRatio": summarize(
                    self.period4_core_ratio, digits=4
                ),
                "consecutiveCoreIdenticalRatio": summarize(
                    self.consecutive_core_ratio, digits=4
                ),
                "adjacentCoreSymmetricChanges": summarize(
                    self.adjacent_core_changes, digits=3
                ),
                "adjacentCoreSimilarity": summarize(
                    self.adjacent_core_similarity, digits=4
                ),
                "period2CoreSimilarity": summarize(
                    self.period2_core_similarity, digits=4
                ),
                "period4CoreSimilarity": summarize(
                    self.period4_core_similarity, digits=4
                ),
                "boundaryCoreSymmetricChanges": {
                    width: summarize(values, digits=3)
                    for width, values in sorted(
                        self.boundary_core_changes.items()
                    )
                },
                "boundaryCoreSimilarity": {
                    width: summarize(values, digits=4)
                    for width, values in sorted(
                        self.boundary_core_similarity.items()
                    )
                },
                "barHitCounts": summarize(
                    self.bar_hit_counts, digits=3
                ),
                "barMeanVelocities": summarize(
                    self.bar_mean_velocities, digits=3
                ),
                "quarterHitSpread": summarize(
                    self.quarter_hit_spread, digits=4
                ),
            },
            "unknownNoteCounts": {
                str(note): count for note, count in self.unknown_notes.most_common()
            },
        }


def dataset_rows(source: DatasetSource) -> list[dict[str, str]]:
    rows = list(csv.DictReader(io.StringIO(source.metadata_text())))
    required = {
        "drummer",
        "id",
        "style",
        "bpm",
        "beat_type",
        "time_signature",
        "midi_filename",
        "duration",
        "split",
    }
    if not rows or not required.issubset(rows[0]):
        missing = sorted(required - set(rows[0] if rows else ()))
        raise ValueError(f"missing GMD metadata columns: {missing}")
    return rows


def analyze(path: Path) -> dict[str, object]:
    source = open_source(path)
    try:
        rows = dataset_rows(source)
        overall = Aggregate()
        styles: dict[str, Aggregate] = defaultdict(Aggregate)
        exact_styles: dict[str, Aggregate] = defaultdict(Aggregate)
        style_beat_types: dict[str, Aggregate] = defaultdict(Aggregate)
        exact_style_beat_types: dict[str, Aggregate] = defaultdict(Aggregate)
        drummers: dict[str, Aggregate] = defaultdict(Aggregate)
        beat_types: dict[str, Aggregate] = defaultdict(Aggregate)
        tempo_bands: dict[str, Aggregate] = defaultdict(Aggregate)
        time_signatures: Counter[str] = Counter()
        errors: list[dict[str, str]] = []
        for row in rows:
            try:
                midi = parse_midi(source.midi_bytes(row["midi_filename"]))
                primary = primary_style(row["style"])
                exact = row["style"].strip().lower()
                beat_type = row["beat_type"].strip().lower()
                aggregates = (
                    overall,
                    styles[primary],
                    exact_styles[exact],
                    style_beat_types[f"{primary}|{beat_type}"],
                    exact_style_beat_types[f"{exact}|{beat_type}"],
                    drummers[row["drummer"].strip().lower()],
                    beat_types[beat_type],
                    tempo_bands[tempo_band(int(row["bpm"]))],
                )
                for aggregate in aggregates:
                    aggregate.add_file(row, midi)
                time_signatures[row["time_signature"].strip()] += 1
            except (OSError, ValueError, KeyError, struct.error) as error:
                errors.append({"id": row.get("id", ""), "error": str(error)})
        return {
            "schema": "jam2-drum-performance-evidence-v2",
            "source": {
                "name": "Google Magenta Groove MIDI Dataset",
                "version": "1.0.0",
                "url": "https://magenta.withgoogle.com/datasets/groove",
                "license": "CC BY 4.0",
                **source.identity(),
            },
            "method": {
                "runtimeDependency": False,
                "purpose": (
                    "Raw evidence for deterministic authored Jam2 performance rules; "
                    "not a learned runtime model or automatic quality score."
                ),
                "timing": (
                    "Each file compares straight-sixteenth and sixth-note-triplet "
                    "positions using only events that materially distinguish the "
                    "grids, requiring a clear triplet majority. Ambiguous files "
                    "remain straight. Signed offsets for both grids and the selected "
                    "grid are all reported; profile design must inspect source style "
                    "before adopting a distribution."
                ),
                "semanticVelocityBands": (
                    "Evidence bands intentionally overlap: ghost p05-p25, normal "
                    "p20-p85, accent p75-p95. Jam2 semantics choose the band before "
                    "deterministic within-band variation."
                ),
                "cooccurrence": "Canonical pieces occupying the same selected-grid slot.",
                "tomDirection": "Direction between consecutive high/mid/floor tom events.",
                "sequence": (
                    "Per-performance active bars are rebuilt from the first downbeat "
                    "through the last known drum event. Quantized instrument-presence "
                    "signatures measure exact recurrence and adjacent change without "
                    "retaining or exporting source patterns. Velocity, articulation, "
                    "and residual timing remain separate aggregate evidence."
                ),
            },
            "metadata": {
                "rows": len(rows),
                "successes": overall.files,
                "errors": len(errors),
                "drummers": len(drummers),
                "primaryStyles": len(styles),
                "exactStyles": len(exact_styles),
                "timeSignatureCounts": dict(time_signatures.most_common()),
            },
            "overall": overall.json(),
            "byPrimaryStyle": {
                key: value.json() for key, value in sorted(styles.items())
            },
            "byExactStyle": {
                key: value.json() for key, value in sorted(exact_styles.items())
            },
            "byPrimaryStyleBeatType": {
                key: value.json()
                for key, value in sorted(style_beat_types.items())
            },
            "byExactStyleBeatType": {
                key: value.json()
                for key, value in sorted(exact_style_beat_types.items())
            },
            "byDrummer": {
                key: value.json() for key, value in sorted(drummers.items())
            },
            "byBeatType": {
                key: value.json() for key, value in sorted(beat_types.items())
            },
            "byTempoBand": {
                key: value.json() for key, value in sorted(tempo_bands.items())
            },
            "errors": errors,
        }
    finally:
        source.close()


def print_summary(result: dict[str, object]) -> None:
    metadata = result["metadata"]
    print(
        "GMD files: "
        f"{metadata['successes']}/{metadata['rows']} parsed, "
        f"{metadata['errors']} errors, "
        f"{metadata['drummers']} drummers, "
        f"{metadata['primaryStyles']} primary styles"
    )
    print("| Style | Files | Beats | Fills | Hits/bar median | Grid (straight/triplet) |")
    print("|---|---:|---:|---:|---:|---:|")
    for style, item in result["byPrimaryStyle"].items():
        grids = item["gridChoices"]
        print(
            f"| {style} | {item['files']} | {item['beats']} | {item['fills']} | "
            f"{item['hitsPerBar'].get('median', 0)} | "
            f"{grids.get('straight-sixteenth', 0)}/"
            f"{grids.get('triplet-sixth', 0)} |"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "dataset",
        type=Path,
        help="Extracted groove-v1.0.0 directory or official zip archive",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.dataset)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print_summary(result)
    return 1 if result["metadata"]["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
