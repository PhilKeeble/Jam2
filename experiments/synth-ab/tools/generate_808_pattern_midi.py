#!/usr/bin/env python3
"""Generate musical MIDI patterns for the remapped DrumSamples 808 Live Set."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

from generate_drum_capture_midi import (
    CHANNEL,
    NOTE_LENGTH_TICKS,
    PPQ,
    base_events,
    encode_track,
    inspect_midi,
    meta_event,
    midi_file,
)


TOTAL_BEATS = 32
TOTAL_TICKS = TOTAL_BEATS * PPQ
SUITE_GAP_BEATS = 8


@dataclass(frozen=True)
class Piece:
    slug: str
    track_name: str
    note: int
    ableton_note_name: str
    captured_wav: str


PIECES = (
    Piece("kick", "Kick", 36, "C1", "DrumSamples Kick.wav"),
    Piece("cross-stick", "cross stick", 37, "C#1", "DrumSamples cross stick.wav"),
    Piece("snare", "Snare", 38, "D1", "DrumSamples Snare.wav"),
    Piece("clap", "percussion - clap", 39, "D#1", "DrumSamples percussion - clap.wav"),
    Piece("ride", "ride", 41, "F1", "DrumSamples ride.wav"),
    Piece("closed-hat", "Closed Hats", 42, "F#1", "DrumSamples Closed Hats.wav"),
    Piece("floor-tom", "Low Tom", 43, "G1", "DrumSamples Low Tom.wav"),
    Piece("mid-tom", "Mid Tom", 45, "A1", "DrumSamples Mid Tom.wav"),
    Piece("open-hat", "Open Hats", 46, "A#1", "DrumSamples Open Hats.wav"),
    Piece("high-tom", "Hi Tom", 47, "B1", "DrumSamples Hi Tom.wav"),
    Piece("crash", "Cymbals", 51, "D#2", "DrumSamples Cymbals.wav"),
)
PIECE_BY_SLUG = {piece.slug: piece for piece in PIECES}


@dataclass(frozen=True)
class Pattern:
    slug: str
    name: str
    purpose: str
    events: dict[str, tuple[tuple[Fraction, int], ...]]


def event_map() -> defaultdict[str, list[tuple[Fraction, int]]]:
    return defaultdict(list)


def add(events: defaultdict[str, list[tuple[Fraction, int]]], piece: str, beat: int | float | Fraction, velocity: int) -> None:
    if piece not in PIECE_BY_SLUG:
        raise ValueError(f"Unknown kit piece: {piece}")
    position = beat if isinstance(beat, Fraction) else Fraction(str(beat))
    if position < 0 or position >= TOTAL_BEATS:
        raise ValueError(f"Event at beat {position} falls outside the eight-bar pattern")
    if not 1 <= velocity <= 127:
        raise ValueError(f"Invalid velocity: {velocity}")
    events[piece].append((position, velocity))


def finish_events(events: defaultdict[str, list[tuple[Fraction, int]]]) -> dict[str, tuple[tuple[Fraction, int], ...]]:
    result: dict[str, tuple[tuple[Fraction, int], ...]] = {}
    for piece in PIECES:
        strongest_by_beat: dict[Fraction, int] = {}
        for beat, velocity in events[piece.slug]:
            strongest_by_beat[beat] = max(velocity, strongest_by_beat.get(beat, 0))
        result[piece.slug] = tuple(sorted(strongest_by_beat.items()))
    return result


def straight_pop() -> Pattern:
    events = event_map()
    for bar in range(8):
        base = bar * 4
        kicks = (0, 2, 2.75) if bar % 2 == 0 else (0, 1.75, 2.5)
        for index, offset in enumerate(kicks):
            add(events, "kick", base + offset, 108 if index == 0 else 91)
        for offset, velocity in ((1, 105), (3, 112)):
            add(events, "snare", base + offset, velocity)
            add(events, "clap", base + offset, velocity - 20)
        open_position = Fraction(7, 2) if bar in (1, 3, 5) else None
        for eighth in range(8):
            position = Fraction(base) + Fraction(eighth, 2)
            if open_position is not None and position == base + open_position:
                continue
            add(events, "closed-hat", position, 72 if eighth % 2 == 0 else 54)
        if open_position is not None:
            add(events, "open-hat", base + open_position, 82)
        for sixteenth in range(16):
            add(events, "closed-hat", Fraction(base) + Fraction(sixteenth, 4), 49 if sixteenth % 4 == 0 else 36)
    add(events, "crash", 0, 116)
    add(events, "crash", 16, 105)
    add(events, "cross-stick", Fraction(21, 2), 59)
    add(events, "cross-stick", Fraction(45, 2), 62)
    for piece, beat, velocity in (
        ("high-tom", 30, 82),
        ("high-tom", 30.5, 88),
        ("mid-tom", 31, 94),
        ("floor-tom", 31.5, 108),
    ):
        add(events, piece, beat, velocity)
    return Pattern("01-straight-808-pop", "Straight 808 pop", "Overall balance, clap/snare layering, hats and a short tom fill", finish_events(events))


def syncopated_electro() -> Pattern:
    events = event_map()
    kick_patterns = (
        (0, 0.75, 2, 2.75),
        (0, 1.5, 2.5, 3.25),
    )
    for bar in range(8):
        base = bar * 4
        for index, offset in enumerate(kick_patterns[bar % 2]):
            add(events, "kick", base + offset, 110 if index == 0 else 88 + (index % 2) * 8)
        for offset in (1, 3):
            add(events, "clap", base + offset, 108 if offset == 3 else 99)
            add(events, "snare", base + offset, 82)
        add(events, "snare", base + 2.75, 44)
        for sixteenth in range(16):
            position = Fraction(base) + Fraction(sixteenth, 4)
            if sixteenth in (7, 15):
                add(events, "open-hat", position, 91 if sixteenth == 15 else 76)
            else:
                add(events, "closed-hat", position, 78 if sixteenth % 4 == 0 else 47 + (sixteenth % 2) * 8)
        for eighth in range(8):
            add(events, "closed-hat", Fraction(base) + Fraction(eighth, 2), 61 if eighth % 2 else 44)
        add(events, "cross-stick", base + 1.75, 55)
    add(events, "crash", 0, 118)
    add(events, "crash", 16, 110)
    add(events, "high-tom", 30.75, 73)
    add(events, "mid-tom", 31, 85)
    add(events, "floor-tom", 31.5, 105)
    return Pattern("02-syncopated-electro", "Syncopated electro", "Dense transient layering, sixteenth hats and syncopated kick", finish_events(events))


def ride_groove() -> Pattern:
    events = event_map()
    ride_velocities = (112, 36, 78, 48, 96, 32, 74, 52)
    for bar in range(8):
        base = bar * 4
        for eighth, velocity in enumerate(ride_velocities):
            add(events, "ride", Fraction(base) + Fraction(eighth, 2), velocity)
        for offset, velocity in ((0, 106), (1.5, 86), (2.5, 93)):
            add(events, "kick", base + offset, velocity)
        add(events, "snare", base + 1, 101)
        add(events, "snare", base + 3, 108)
        add(events, "cross-stick", base + 2.75, 52)
        if bar % 2:
            add(events, "clap", base + 3, 67)
        for offset in (0.75, 1.75, 2.75, 3.75):
            add(events, "closed-hat", base + offset, 47)
    add(events, "crash", 0, 112)
    add(events, "crash", 16, 104)
    add(events, "high-tom", 30.5, 74)
    add(events, "mid-tom", 31, 87)
    add(events, "floor-tom", 31.5, 102)
    return Pattern("03-ride-groove", "Ride groove", "Single-ride ghost, normal and accent response plus accumulated metallic tails", finish_events(events))


def tom_fill_context() -> Pattern:
    events = event_map()
    for bar in range(6):
        base = bar * 4
        for offset, velocity in ((0, 110), (1.5, 84), (3, 96)):
            add(events, "kick", base + offset, velocity)
        add(events, "snare", base + 2, 112)
        add(events, "clap", base + 2, 78)
        for eighth in range(8):
            add(events, "closed-hat", Fraction(base) + Fraction(eighth, 2), 70 if eighth % 2 == 0 else 51)
        if bar % 2 == 1:
            add(events, "open-hat", base + 3.5, 80)
        for offset in (0.75, 1.75, 2.75, 3.75):
            add(events, "closed-hat", base + offset, 43)
    add(events, "crash", 0, 119)
    add(events, "crash", 16, 108)
    add(events, "kick", 24, 110)
    add(events, "snare", 26, 105)
    for piece, beat, velocity in (
        ("high-tom", 24.5, 70),
        ("high-tom", 25, 76),
        ("mid-tom", 25.5, 82),
        ("floor-tom", 26.5, 90),
        ("high-tom", 27, 88),
        ("mid-tom", 27.5, 94),
        ("floor-tom", 28, 101),
        ("floor-tom", 28.5, 108),
        ("high-tom", 29, 92),
        ("mid-tom", 29.25, 97),
        ("floor-tom", 29.5, 104),
        ("high-tom", 30, 98),
        ("mid-tom", 30.5, 106),
        ("floor-tom", 31, 116),
        ("floor-tom", 31.5, 124),
    ):
        add(events, piece, beat, velocity)
    add(events, "ride", 27.75, 74)
    add(events, "open-hat", 30.75, 86)
    return Pattern("04-tom-fill-context", "Tom fill context", "Tom level, pitch ordering and tail buildup after six bars of half-time context", finish_events(events))


PATTERNS = (straight_pop(), syncopated_electro(), ride_groove(), tom_fill_context())


def make_midi(pattern: Pattern, selected_piece: Piece | None) -> bytes:
    label = selected_piece.track_name if selected_piece is not None else "full kit"
    events = base_events(
        f"Jam2 808 {pattern.name} - {label}",
        f"Eight bars at 120 BPM; ALS-derived 808 mapping; {pattern.purpose}",
    )
    events.append((0, 10, meta_event(0x06, pattern.name.encode("ascii"))))
    pieces = (selected_piece,) if selected_piece is not None else PIECES
    for piece in pieces:
        for beat, velocity in pattern.events[piece.slug]:
            tick_fraction = beat * PPQ
            if tick_fraction.denominator != 1:
                raise ValueError(f"{pattern.slug}: beat {beat} does not land on the {PPQ} PPQ grid")
            tick = tick_fraction.numerator
            events.append((tick, 20, bytes((0x90 | CHANNEL, piece.note, velocity))))
            events.append((tick + NOTE_LENGTH_TICKS, 30, bytes((0x80 | CHANNEL, piece.note, 0))))
    return midi_file(encode_track(events, TOTAL_TICKS))


def suite_offsets() -> tuple[int, ...]:
    stride = TOTAL_BEATS + SUITE_GAP_BEATS
    return tuple(index * stride for index in range(len(PATTERNS)))


def suite_total_beats() -> int:
    return suite_offsets()[-1] + TOTAL_BEATS


def make_suite_midi(selected_piece: Piece | None) -> bytes:
    label = selected_piece.track_name if selected_piece is not None else "full kit"
    events = base_events(
        f"Jam2 808 comparison suite - {label}",
        "Four eight-bar patterns at 120 BPM with two silent bars between patterns",
    )
    pieces = (selected_piece,) if selected_piece is not None else PIECES
    for pattern, offset in zip(PATTERNS, suite_offsets()):
        events.append((offset * PPQ, 10, meta_event(0x06, pattern.name.encode("ascii"))))
        for piece in pieces:
            for beat, velocity in pattern.events[piece.slug]:
                tick = int((beat + offset) * PPQ)
                events.append((tick, 20, bytes((0x90 | CHANNEL, piece.note, velocity))))
                events.append((tick + NOTE_LENGTH_TICKS, 30, bytes((0x80 | CHANNEL, piece.note, 0))))
    return midi_file(encode_track(events, suite_total_beats() * PPQ))


def expected_events(pattern: Pattern, selected_piece: Piece | None) -> tuple[tuple[int, int, int], ...]:
    pieces = (selected_piece,) if selected_piece is not None else PIECES
    expected: list[tuple[int, int, int]] = []
    for piece in pieces:
        expected.extend((int(beat * PPQ), piece.note, velocity) for beat, velocity in pattern.events[piece.slug])
    return tuple(sorted(expected))


def validate_file(path: Path, pattern: Pattern, selected_piece: Piece | None) -> dict[str, object]:
    data = path.read_bytes()
    inspection = inspect_midi(data)
    note_ons = inspection["note_ons"]
    assert isinstance(note_ons, list)
    actual = tuple(sorted((event[0], event[2], event[3]) for event in note_ons))
    expected = expected_events(pattern, selected_piece)
    if actual != expected:
        raise ValueError(f"{path}: note and velocity events differ from the pattern definition")
    if inspection["note_offs"] != len(expected) or inspection["end_tick"] != TOTAL_TICKS:
        raise ValueError(f"{path}: note-off count or eight-bar duration differs")
    if any(event[1] != CHANNEL for event in note_ons):
        raise ValueError(f"{path}: events are not on MIDI channel 10")
    return {
        "file": path.name if selected_piece is None else f"stems/{path.name}",
        "piece": selected_piece.slug if selected_piece is not None else "full-kit",
        "hitCount": len(expected),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def expected_suite_events(selected_piece: Piece | None) -> tuple[tuple[int, int, int], ...]:
    pieces = (selected_piece,) if selected_piece is not None else PIECES
    expected: list[tuple[int, int, int]] = []
    for pattern, offset in zip(PATTERNS, suite_offsets()):
        for piece in pieces:
            expected.extend(
                (int((beat + offset) * PPQ), piece.note, velocity)
                for beat, velocity in pattern.events[piece.slug]
            )
    return tuple(sorted(expected))


def validate_suite_file(path: Path, selected_piece: Piece | None) -> dict[str, object]:
    data = path.read_bytes()
    inspection = inspect_midi(data)
    note_ons = inspection["note_ons"]
    assert isinstance(note_ons, list)
    actual = tuple(sorted((event[0], event[2], event[3]) for event in note_ons))
    expected = expected_suite_events(selected_piece)
    if actual != expected:
        raise ValueError(f"{path}: note, velocity, or timing events differ from the suite definition")
    if inspection["note_offs"] != len(expected) or inspection["end_tick"] != suite_total_beats() * PPQ:
        raise ValueError(f"{path}: note-off count or comparison-suite duration differs")
    if any(event[1] != CHANNEL for event in note_ons):
        raise ValueError(f"{path}: events are not on MIDI channel 10")
    return {
        "file": path.name if selected_piece is None else f"stems/{path.name}",
        "piece": selected_piece.slug if selected_piece is not None else "full-kit",
        "hitCount": len(expected),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def write_pack(output: Path) -> None:
    for pattern in PATTERNS:
        pattern_dir = output / "patterns" / pattern.slug
        stems_dir = pattern_dir / "stems"
        stems_dir.mkdir(parents=True, exist_ok=True)
        expected_stems = {
            f"{piece.slug}-note-{piece.note}.mid"
            for piece in PIECES
        }
        for path in stems_dir.glob("*.mid"):
            if path.name not in expected_stems:
                path.unlink()
        (pattern_dir / "full-kit.mid").write_bytes(make_midi(pattern, None))
        for piece in PIECES:
            (stems_dir / f"{piece.slug}-note-{piece.note}.mid").write_bytes(make_midi(pattern, piece))
    suite_dir = output / "comparison-suite"
    suite_stems_dir = suite_dir / "stems"
    suite_stems_dir.mkdir(parents=True, exist_ok=True)
    expected_stems = {
        f"{piece.slug}-note-{piece.note}.mid"
        for piece in PIECES
    }
    for path in suite_stems_dir.glob("*.mid"):
        if path.name not in expected_stems:
            path.unlink()
    (suite_dir / "full-kit.mid").write_bytes(make_suite_midi(None))
    for piece in PIECES:
        (suite_stems_dir / f"{piece.slug}-note-{piece.note}.mid").write_bytes(make_suite_midi(piece))


def validate_pack(output: Path, write_manifest: bool) -> dict[str, object]:
    pattern_summaries = []
    for pattern in PATTERNS:
        pattern_dir = output / "patterns" / pattern.slug
        files = [validate_file(pattern_dir / "full-kit.mid", pattern, None)]
        for piece in PIECES:
            files.append(validate_file(pattern_dir / "stems" / f"{piece.slug}-note-{piece.note}.mid", pattern, piece))
        pattern_summaries.append(
            {
                "id": pattern.slug,
                "name": pattern.name,
                "purpose": pattern.purpose,
                "bars": 8,
                "durationSeconds": 16.0,
                "files": files,
            }
        )
    suite_dir = output / "comparison-suite"
    suite_files = [validate_suite_file(suite_dir / "full-kit.mid", None)]
    for piece in PIECES:
        suite_files.append(validate_suite_file(suite_dir / "stems" / f"{piece.slug}-note-{piece.note}.mid", piece))
    manifest = {
        "schema": "jam2-als-mapped-pattern-midi-v1",
        "sourceLiveSet": "DrumSamples.als",
        "tempoBpm": 120,
        "midiChannel": CHANNEL + 1,
        "mapping": [
            {
                "piece": piece.slug,
                "abletonTrack": piece.track_name,
                "midiNote": piece.note,
                "abletonNoteName": piece.ableton_note_name,
                "capturedWav": piece.captured_wav,
            }
            for piece in PIECES
        ],
        "rideVelocityUse": {
            "ghost": "Lower-velocity hits on the single ride sample",
            "normal": "Normal-velocity hits on the single ride sample",
            "accent": "Higher-velocity hits on the single ride sample",
        },
        "comparisonSuite": {
            "durationBars": suite_total_beats() // 4,
            "durationSeconds": suite_total_beats() / 2,
            "silentBarsBetweenPatterns": SUITE_GAP_BEATS // 4,
            "patternStarts": [
                {"pattern": pattern.slug, "startBar": offset // 4 + 1}
                for pattern, offset in zip(PATTERNS, suite_offsets())
            ],
            "files": suite_files,
        },
        "patterns": pattern_summaries,
    }
    serialized = json.dumps(manifest, indent=2) + "\n"
    manifest_path = output / "manifest.json"
    if write_manifest:
        manifest_path.write_text(serialized, encoding="utf-8")
    elif not manifest_path.exists() or manifest_path.read_text(encoding="utf-8") != serialized:
        raise ValueError("manifest.json is missing or differs from the pattern files")
    return manifest


def main() -> int:
    default_output = Path(__file__).resolve().parents[1] / "capture-midi" / "808-project-v1"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--check", action="store_true", help="validate without rewriting files")
    args = parser.parse_args()

    output = args.output.resolve()
    if not args.check:
        write_pack(output)
    manifest = validate_pack(output, write_manifest=not args.check)
    action = "Validated" if args.check else "Generated and validated"
    print(f"{action} {len(manifest['patterns'])} eight-bar 808 pattern sets in {output}")
    for pattern in manifest["patterns"]:
        full_kit = pattern["files"][0]
        print(f"  {pattern['id']}: {full_kit['hitCount']} full-kit hits, {len(PIECES)} aligned stems")
    print(
        f"  comparison-suite: {manifest['comparisonSuite']['durationBars']} bars, "
        f"{manifest['comparisonSuite']['files'][0]['hitCount']} full-kit hits, {len(PIECES)} aligned stems"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
