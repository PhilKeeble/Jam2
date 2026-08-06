#!/usr/bin/env python3
"""Generate and validate the Jam2 melodic reference-capture MIDI pack.

The generator intentionally uses only the Python standard library.  The MIDI
files are build artifacts, while this file is the inspectable source of truth.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path


PPQ = 480
TEMPO_BPM = 120
TEMPO_US_PER_QUARTER = 60_000_000 // TEMPO_BPM
PRE_ROLL_BEATS = 4
CAPTURE_LOW = 28
CAPTURE_HIGH = 84
VELOCITIES = (20, 50, 80, 110, 127)
MELODIC_CHANNEL = 0
DRUM_CHANNEL = 9


Event = tuple[int, int, bytes]


@dataclass(frozen=True)
class Sequence:
    slug: str
    name: str
    purpose: str
    events: tuple[Event, ...]
    end_tick: int


@dataclass(frozen=True)
class Artifact:
    path: str
    category: str
    purpose: str
    data: bytes


def variable_length(value: int) -> bytes:
    if value < 0:
        raise ValueError("MIDI delta times cannot be negative")
    encoded = [value & 0x7F]
    value >>= 7
    while value:
        encoded.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(encoded))


def meta_event(kind: int, payload: bytes) -> bytes:
    return bytes((0xFF, kind)) + variable_length(len(payload)) + payload


def marker(label: str) -> bytes:
    return meta_event(0x06, label.encode("ascii"))


def beat(value: int | float | Fraction) -> int:
    fraction = value if isinstance(value, Fraction) else Fraction(str(value))
    ticks = fraction * PPQ
    if ticks.denominator != 1:
        raise ValueError(f"Beat position {value} is not representable at {PPQ} PPQ")
    return ticks.numerator


def note_name(midi: int, ableton: bool = False) -> str:
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    octave_offset = 2 if ableton else 1
    return f"{names[midi % 12]}{midi // 12 - octave_offset}"


def note_events(
    start_tick: int,
    duration_ticks: int,
    notes: int | tuple[int, ...] | list[int],
    velocity: int,
    channel: int = MELODIC_CHANNEL,
) -> list[Event]:
    pitches = (notes,) if isinstance(notes, int) else tuple(notes)
    if duration_ticks <= 0:
        raise ValueError("Note duration must be positive")
    if not pitches or any(note < 0 or note > 127 for note in pitches):
        raise ValueError("Invalid MIDI note")
    if not 1 <= velocity <= 127:
        raise ValueError("Invalid MIDI velocity")
    result: list[Event] = []
    for pitch in pitches:
        result.append((start_tick, 30, bytes((0x90 | channel, pitch, velocity))))
        result.append((start_tick + duration_ticks, 20, bytes((0x80 | channel, pitch, 0))))
    return result


def control_event(tick: int, controller: int, value: int, channel: int = MELODIC_CHANNEL) -> Event:
    return (tick, 25, bytes((0xB0 | channel, controller, value)))


def pressure_event(tick: int, value: int, channel: int = MELODIC_CHANNEL) -> Event:
    return (tick, 25, bytes((0xD0 | channel, value)))


def encode_track(events: list[Event] | tuple[Event, ...], end_tick: int) -> bytes:
    ordered = list(events) + [(end_tick, 99, meta_event(0x2F, b""))]
    ordered.sort(key=lambda event: (event[0], event[1], event[2]))
    previous_tick = 0
    encoded = bytearray()
    for tick, _order, payload in ordered:
        if tick < previous_tick or tick > end_tick:
            raise ValueError("MIDI event lies outside its track")
        encoded.extend(variable_length(tick - previous_tick))
        encoded.extend(payload)
        previous_tick = tick
    return bytes(encoded)


def track_chunk(track: bytes) -> bytes:
    return b"MTrk" + struct.pack(">I", len(track)) + track


def base_meta(name: str, description: str, include_tempo: bool = True) -> list[Event]:
    result = [
        (0, 0, meta_event(0x03, name.encode("ascii"))),
        (0, 1, meta_event(0x01, description.encode("ascii"))),
    ]
    if include_tempo:
        result.extend(
            [
                (0, 2, meta_event(0x51, TEMPO_US_PER_QUARTER.to_bytes(3, "big"))),
                (0, 3, meta_event(0x58, bytes((4, 2, 24, 8)))),
                (0, 4, meta_event(0x59, bytes((0, 1)))),  # A minor / C major.
            ]
        )
    return result


def format_zero(sequence: Sequence) -> bytes:
    events = base_meta(sequence.name, sequence.purpose) + list(sequence.events)
    track = encode_track(events, sequence.end_tick)
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    return header + track_chunk(track)


def format_one(name: str, purpose: str, tracks: tuple[Sequence, ...], end_tick: int) -> bytes:
    conductor = encode_track(base_meta(name, purpose), end_tick)
    chunks = [track_chunk(conductor)]
    for sequence in tracks:
        events = base_meta(sequence.name, sequence.purpose, include_tempo=False)
        events.extend(sequence.events)
        chunks.append(track_chunk(encode_track(events, end_tick)))
    header = b"MThd" + struct.pack(">IHHH", 6, 1, len(chunks), PPQ)
    return header + b"".join(chunks)


def chromatic_sequence() -> Sequence:
    events: list[Event] = [(beat(PRE_ROLL_BEATS), 10, marker("Chromatic E1-C6 / Ableton E0-C5"))]
    cursor = beat(PRE_ROLL_BEATS)
    for midi in range(CAPTURE_LOW, CAPTURE_HIGH + 1):
        events.extend(note_events(cursor, beat(1), midi, 80))
        cursor += beat(4)
    end_tick = cursor + beat(4)
    return Sequence(
        "01-chromatic-range",
        "Jam2 chromatic range v1",
        "Every semitone MIDI 28-84 at velocity 80; one-beat notes with release space",
        tuple(events),
        end_tick,
    )


def velocity_sequence() -> Sequence:
    events: list[Event] = []
    cursor = beat(PRE_ROLL_BEATS)
    for midi, label in ((36, "low C2"), (60, "middle C4"), (81, "high A5")):
        events.append((cursor, 10, marker(f"{label}: velocities 20 50 80 110 127 x4")))
        for velocity in VELOCITIES:
            for _repeat in range(4):
                events.extend(note_events(cursor, beat(1), midi, velocity))
                cursor += beat(4)
        cursor += beat(4)
    end_tick = cursor + beat(4)
    return Sequence(
        "02-velocity-response",
        "Jam2 velocity response v1",
        "Four repeats of five velocities at representative low, middle, and high notes",
        tuple(events),
        end_tick,
    )


def duration_sequence() -> Sequence:
    events: list[Event] = []
    cursor = beat(PRE_ROLL_BEATS)
    durations = ((Fraction(1, 4), "125 ms"), (2, "1 second"), (8, "4 seconds"))
    for midi, label in ((36, "low C2"), (60, "middle C4"), (81, "high A5")):
        events.append((cursor, 10, marker(f"{label}: staccato medium held")))
        for length_beats, duration_label in durations:
            events.append((cursor, 11, marker(duration_label)))
            events.extend(note_events(cursor, beat(length_beats), midi, 80))
            cursor += beat(length_beats) + beat(8)
        cursor += beat(4)
    end_tick = cursor
    return Sequence(
        "03-duration-release",
        "Jam2 duration and release v1",
        "125 ms, one-second, and four-second notes in three registers with four-second tails",
        tuple(events),
        end_tick,
    )


def repeated_sequence() -> Sequence:
    events: list[Event] = []
    cursor = beat(PRE_ROLL_BEATS)
    for spacing, duration, label in (
        (Fraction(1, 4), Fraction(1, 8), "sixteenth retriggers"),
        (Fraction(1, 2), Fraction(1, 4), "eighth retriggers"),
        (1, Fraction(1, 2), "quarter retriggers"),
    ):
        events.append((cursor, 10, marker(label)))
        for repeat in range(16):
            velocity = (72, 80, 88, 80)[repeat % 4]
            events.extend(note_events(cursor, beat(duration), 60, velocity))
            cursor += beat(spacing)
        cursor += beat(8)
    end_tick = cursor
    return Sequence(
        "04-repeated-notes",
        "Jam2 repeated notes v1",
        "Sixteenth, eighth, and quarter-note retriggers expose phase reset and voice stealing",
        tuple(events),
        end_tick,
    )


def polyphony_sequence() -> Sequence:
    events: list[Event] = []
    cursor = beat(PRE_ROLL_BEATS)
    groups: tuple[tuple[str, tuple[tuple[int, ...], ...]], ...] = (
        ("low intervals", ((36, 39), (36, 40), (36, 43), (36, 48))),
        ("middle intervals", ((60, 63), (60, 64), (60, 67), (60, 72))),
        ("high intervals", ((72, 75), (72, 76), (72, 79), (72, 84))),
        ("low chords", ((36, 40, 43), (36, 39, 43), (36, 43, 52), (36, 40, 43, 46))),
        ("middle chords", ((60, 64, 67), (57, 60, 64), (60, 62, 67), (60, 65, 67), (60, 64, 67, 70))),
        ("high chords", ((72, 76, 79), (69, 72, 76), (72, 74, 79), (72, 77, 79), (67, 72, 76, 79))),
        ("inversions and open voicings", ((48, 55, 64), (52, 55, 60), (55, 60, 64), (48, 60, 64, 67))),
    )
    for label, voicings in groups:
        events.append((cursor, 10, marker(label)))
        for notes in voicings:
            events.extend(note_events(cursor, beat(4), notes, 80))
            cursor += beat(8)
        cursor += beat(4)
    end_tick = cursor
    return Sequence(
        "05-polyphony-chords",
        "Jam2 polyphony and chords v1",
        "Intervals, triads, sevenths, inversions, and open voicings across the playable range",
        tuple(events),
        end_tick,
    )


def legato_sequence() -> Sequence:
    events: list[Event] = []
    scale = (60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60)
    cursor = beat(PRE_ROLL_BEATS)
    events.append((cursor, 10, marker("detached scale")))
    for midi in scale:
        events.extend(note_events(cursor, beat(Fraction(3, 4)), midi, 82))
        cursor += beat(1)
    cursor += beat(6)
    events.append((cursor, 10, marker("overlapping legato scale")))
    for midi in scale:
        events.extend(note_events(cursor, beat(Fraction(5, 4)), midi, 82))
        cursor += beat(1)
    cursor += beat(8)
    events.append((cursor, 10, marker("wide glide test")))
    for midi in (48, 60, 72, 55, 67, 79, 60):
        events.extend(note_events(cursor, beat(Fraction(3, 2)), midi, 88))
        cursor += beat(1)
    end_tick = cursor + beat(8)
    return Sequence(
        "06-legato-glide",
        "Jam2 legato and glide v1",
        "Detached and overlapping scales plus wide overlapping leaps for mono and glide patches",
        tuple(events),
        end_tick,
    )


def expression_sequence() -> Sequence:
    events: list[Event] = []
    cursor = beat(PRE_ROLL_BEATS)
    events.extend((control_event(0, 1, 0), control_event(0, 11, 127), control_event(0, 64, 0), pressure_event(0, 0)))

    events.append((cursor, 10, marker("CC1 modulation sweep")))
    events.extend(note_events(cursor, beat(12), 60, 88))
    for offset, value in ((0, 0), (2, 24), (4, 48), (6, 72), (8, 96), (10, 127)):
        events.append(control_event(cursor + beat(offset), 1, value))
    cursor += beat(20)
    events.append(control_event(cursor, 1, 0))

    events.append((cursor, 10, marker("channel aftertouch sweep")))
    events.extend(note_events(cursor, beat(12), 60, 88))
    for offset, value in ((0, 0), (2, 24), (4, 48), (6, 72), (8, 96), (10, 127)):
        events.append(pressure_event(cursor + beat(offset), value))
    cursor += beat(20)
    events.append(pressure_event(cursor, 0))

    events.append((cursor, 10, marker("expression CC11 down and up")))
    events.extend(note_events(cursor, beat(16), 60, 88))
    for offset, value in ((0, 127), (3, 96), (6, 64), (8, 32), (10, 64), (13, 96), (15, 127)):
        events.append(control_event(cursor + beat(offset), 11, value))
    cursor += beat(24)

    events.append((cursor, 10, marker("sustain pedal chord release")))
    events.append(control_event(cursor, 64, 127))
    events.extend(note_events(cursor, beat(2), (60, 64, 67), 80))
    events.append(control_event(cursor + beat(8), 64, 0))
    cursor += beat(16)
    events.extend((control_event(cursor, 1, 0), control_event(cursor, 11, 127), control_event(cursor, 64, 0), pressure_event(cursor, 0)))
    return Sequence(
        "07-optional-expression",
        "Jam2 optional expression controls v1",
        "Optional CC1, channel aftertouch, CC11, and sustain-pedal tests; controller state is reset",
        tuple(events),
        cursor + beat(4),
    )


def combined_capture(sequences: tuple[Sequence, ...]) -> Sequence:
    events: list[Event] = []
    cursor = 0
    for sequence in sequences:
        events.append((cursor, 5, marker(sequence.name)))
        events.extend((tick + cursor, order, payload) for tick, order, payload in sequence.events)
        cursor += sequence.end_tick + beat(8)
    return Sequence(
        "00-full-spectrum",
        "Jam2 complete melodic capture v1",
        "All seven melodic replication tests in one render, separated by four seconds",
        tuple(events),
        cursor,
    )


def add_role_note(events: list[Event], start: int | float | Fraction, duration: int | float | Fraction,
                  notes: int | tuple[int, ...], velocity: int, channel: int) -> None:
    events.extend(note_events(beat(start), beat(duration), notes, velocity, channel))


def context_chords() -> Sequence:
    events: list[Event] = [(0, 10, marker("16-bar A minor context: chords"))]
    close = ((57, 60, 64), (53, 57, 60), (55, 60, 64), (55, 59, 62))
    inversions = ((60, 64, 69), (60, 65, 69), (55, 60, 64), (50, 55, 59))
    open_voicings = ((45, 52, 60, 64), (41, 48, 57, 60), (48, 55, 64, 67), (43, 50, 59, 62))
    for bar_index in range(16):
        start = bar_index * 4
        chord = close[bar_index % 4]
        if bar_index < 4:
            add_role_note(events, start, Fraction(15, 4), chord, 78 + bar_index * 2, 0)
        elif bar_index < 8:
            chord = inversions[bar_index % 4]
            for offset, duration, velocity in ((0, Fraction(1, 2), 86), (Fraction(3, 2), Fraction(1, 2), 68), (Fraction(5, 2), 1, 82)):
                add_role_note(events, start + offset, duration, chord, velocity, 0)
        elif bar_index < 12:
            add_role_note(events, start, Fraction(15, 4), open_voicings[bar_index % 4], 72, 0)
        else:
            for offset, duration, velocity in ((0, Fraction(7, 4), 84), (2, Fraction(3, 4), 72), (3, Fraction(3, 4), 88)):
                add_role_note(events, start + offset, duration, chord, velocity, 0)
    return Sequence("Chords", "Jam2 context - chords", "Sustains, stabs, inversions, and open voicings", tuple(events), beat(64))


def context_melody() -> Sequence:
    events: list[Event] = [(0, 10, marker("16-bar A minor context: melody"))]
    phrases = (
        ((0, 69, 1, 88), (1, 72, Fraction(1, 2), 78), (Fraction(3, 2), 76, Fraction(1, 2), 92), (2, 74, 1, 84), (3, 72, 1, 76)),
        ((0, 69, 1, 82), (1, 67, 1, 76), (2, 65, Fraction(1, 2), 80), (Fraction(5, 2), 69, Fraction(3, 2), 94)),
        ((0, 67, Fraction(1, 2), 80), (Fraction(1, 2), 72, Fraction(1, 2), 86), (1, 76, 1, 96), (2, 79, 1, 102), (3, 76, 1, 84)),
        ((0, 74, 1, 88), (1, 71, Fraction(1, 2), 78), (Fraction(3, 2), 69, Fraction(1, 2), 76), (2, 67, 1, 82), (3, 64, 1, 72)),
    )
    for bar_index in range(16):
        variation = 0 if bar_index < 8 else (2 if bar_index in (10, 14) else 0)
        for offset, midi, duration, velocity in phrases[bar_index % 4]:
            adjusted = min(81, midi + variation)
            add_role_note(events, bar_index * 4 + offset, duration, adjusted, min(112, velocity + (4 if bar_index >= 12 else 0)), 1)
    return Sequence("Melody", "Jam2 context - melody", "Hook, answers, register lift, and velocity contour", tuple(events), beat(64))


def context_bass() -> Sequence:
    events: list[Event] = [(0, 10, marker("16-bar A minor context: bass"))]
    roots = (45, 41, 48, 43)
    fifths = (52, 48, 55, 50)
    for bar_index in range(16):
        start = bar_index * 4
        root = roots[bar_index % 4]
        fifth = fifths[bar_index % 4]
        if bar_index < 4:
            pattern = ((0, root, Fraction(3, 2), 94), (2, fifth, 1, 78), (3, root, 1, 86))
        elif bar_index < 12:
            pattern = tuple((Fraction(step, 2), root if step in (0, 1, 4, 6) else fifth, Fraction(3, 8), 96 if step == 0 else 76 + (step % 2) * 8) for step in range(8))
        else:
            approach = roots[(bar_index + 1) % 4] - 1
            pattern = ((0, root, 1, 98), (1, fifth, 1, 82), (2, root, 1, 88), (3, approach, Fraction(3, 4), 74))
        for offset, midi, duration, velocity in pattern:
            add_role_note(events, start + offset, duration, midi, velocity, 2)
    return Sequence("Bass", "Jam2 context - bass", "Roots, fifths, repeated eighths, and approach notes", tuple(events), beat(64))


def context_support() -> Sequence:
    events: list[Event] = [(0, 10, marker("16-bar A minor context: support"))]
    arpeggios = ((57, 60, 64, 69), (53, 57, 60, 65), (55, 60, 64, 67), (55, 59, 62, 67))
    for bar_index in range(16):
        start = bar_index * 4
        notes = arpeggios[bar_index % 4]
        if bar_index < 4:
            add_role_note(events, start + 2, Fraction(3, 2), (notes[1], notes[3]), 58, 3)
        elif bar_index < 12:
            for step, midi in enumerate(notes):
                add_role_note(events, start + Fraction(step * 2 + 1, 2), Fraction(3, 8), midi, 58 + step * 5, 3)
        else:
            add_role_note(events, start, Fraction(7, 4), (notes[0], notes[2]), 66, 3)
            add_role_note(events, start + Fraction(5, 2), Fraction(5, 4), (notes[1], notes[3]), 72, 3)
    return Sequence("Support", "Jam2 context - support", "Quiet answers, offbeat arpeggios, and chord-tone layers", tuple(events), beat(64))


def context_drums() -> Sequence:
    events: list[Event] = [(0, 10, marker("16-bar context: General MIDI drums"))]
    for bar_index in range(16):
        start = bar_index * 4
        if bar_index in (0, 8):
            add_role_note(events, start, Fraction(1, 4), 49, 112, DRUM_CHANNEL)
        for offset, velocity in ((0, 110), (Fraction(3, 2), 84), (Fraction(5, 2), 92)):
            add_role_note(events, start + offset, Fraction(1, 8), 36, velocity, DRUM_CHANNEL)
        for offset in (1, 3):
            add_role_note(events, start + offset, Fraction(1, 8), 38, 108 if offset == 3 else 100, DRUM_CHANNEL)
        for eighth in range(8):
            position = start + Fraction(eighth, 2)
            if eighth == 7 and bar_index % 4 == 3:
                add_role_note(events, position, Fraction(1, 4), 46, 82, DRUM_CHANNEL)
            else:
                add_role_note(events, position, Fraction(1, 8), 42, 72 if eighth % 2 == 0 else 52, DRUM_CHANNEL)
        if 8 <= bar_index < 12:
            for sixteenth in range(1, 16, 2):
                add_role_note(events, start + Fraction(sixteenth, 4), Fraction(1, 16), 42, 38, DRUM_CHANNEL)
    for midi, position, velocity in ((50, Fraction(125, 2), 82), (47, 63, 94), (43, Fraction(127, 2), 110)):
        add_role_note(events, position, Fraction(1, 8), midi, velocity, DRUM_CHANNEL)
    return Sequence("Drums", "Jam2 context - drums", "GM kick, snare, hats, crash, and closing tom fill", tuple(events), beat(64))


def read_variable_length(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for _ in range(4):
        if offset >= len(data):
            raise ValueError("Truncated MIDI variable-length integer")
        byte_value = data[offset]
        offset += 1
        value = (value << 7) | (byte_value & 0x7F)
        if not byte_value & 0x80:
            return value, offset
    raise ValueError("Invalid MIDI variable-length integer")


def inspect_track(data: bytes) -> dict[str, object]:
    offset = 0
    tick_value = 0
    running_status: int | None = None
    notes: list[tuple[int, int, int, int]] = []
    note_offs = 0
    controls = 0
    pressure = 0
    markers: list[str] = []
    track_name = ""
    end_tick: int | None = None
    while offset < len(data):
        delta, offset = read_variable_length(data, offset)
        tick_value += delta
        status = data[offset]
        if status < 0x80:
            if running_status is None:
                raise ValueError("Running status used before a channel status")
            status = running_status
        else:
            offset += 1
        if status == 0xFF:
            running_status = None
            kind = data[offset]
            offset += 1
            length, offset = read_variable_length(data, offset)
            payload = data[offset:offset + length]
            offset += length
            if kind == 0x03:
                track_name = payload.decode("ascii")
            elif kind == 0x06:
                markers.append(payload.decode("ascii"))
            elif kind == 0x2F:
                end_tick = tick_value
                break
        elif status in (0xF0, 0xF7):
            running_status = None
            length, offset = read_variable_length(data, offset)
            offset += length
        else:
            running_status = status
            command = status & 0xF0
            channel = status & 0x0F
            size = 1 if command in (0xC0, 0xD0) else 2
            payload = data[offset:offset + size]
            if len(payload) != size:
                raise ValueError("Truncated MIDI channel event")
            offset += size
            if command == 0x90 and payload[1] != 0:
                notes.append((tick_value, channel, payload[0], payload[1]))
            elif command == 0x80 or (command == 0x90 and payload[1] == 0):
                note_offs += 1
            elif command == 0xB0:
                controls += 1
            elif command == 0xD0:
                pressure += 1
    if end_tick is None:
        raise ValueError("MIDI track has no end-of-track event")
    return {
        "trackName": track_name,
        "endTick": end_tick,
        "notes": notes,
        "noteOffCount": note_offs,
        "controlChangeCount": controls,
        "channelPressureCount": pressure,
        "markers": markers,
    }


def inspect_midi(data: bytes) -> dict[str, object]:
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("Missing MIDI header")
    header_length, midi_format, track_count, division = struct.unpack(">IHHH", data[4:14])
    if header_length != 6 or midi_format not in (0, 1) or division != PPQ:
        raise ValueError("Unexpected MIDI header")
    offset = 14
    tracks: list[dict[str, object]] = []
    for _ in range(track_count):
        if data[offset:offset + 4] != b"MTrk":
            raise ValueError("Missing MIDI track chunk")
        length = struct.unpack(">I", data[offset + 4:offset + 8])[0]
        start = offset + 8
        end = start + length
        if end > len(data):
            raise ValueError("Truncated MIDI track")
        tracks.append(inspect_track(data[start:end]))
        offset = end
    if offset != len(data):
        raise ValueError("Unexpected bytes after MIDI tracks")
    return {"format": midi_format, "trackCount": track_count, "tracks": tracks}


def seconds_for_ticks(ticks: int) -> float:
    return ticks * TEMPO_US_PER_QUARTER / PPQ / 1_000_000


def artifact_manifest(artifact: Artifact) -> dict[str, object]:
    inspection = inspect_midi(artifact.data)
    tracks = inspection["tracks"]
    assert isinstance(tracks, list)
    notes = [note for track in tracks for note in track["notes"]]
    note_off_count = sum(int(track["noteOffCount"]) for track in tracks)
    if note_off_count != len(notes):
        raise ValueError(f"{artifact.path}: note-on/note-off count mismatch")
    end_ticks = {int(track["endTick"]) for track in tracks}
    if len(end_ticks) != 1:
        raise ValueError(f"{artifact.path}: tracks do not have aligned durations")
    end_tick = next(iter(end_ticks))
    pitches = [event[2] for event in notes]
    velocities = [event[3] for event in notes]
    channels = sorted({event[1] + 1 for event in notes})
    return {
        "file": artifact.path.replace("\\", "/"),
        "category": artifact.category,
        "purpose": artifact.purpose,
        "midiFormat": inspection["format"],
        "trackCount": inspection["trackCount"],
        "trackNames": [track["trackName"] for track in tracks],
        "durationSeconds": seconds_for_ticks(end_tick),
        "noteOnCount": len(notes),
        "noteRange": None if not pitches else {
            "minimum": min(pitches),
            "maximum": max(pitches),
            "scientific": f"{note_name(min(pitches))}-{note_name(max(pitches))}",
            "ableton": f"{note_name(min(pitches), True)}-{note_name(max(pitches), True)}",
        },
        "velocityRange": None if not velocities else {"minimum": min(velocities), "maximum": max(velocities)},
        "midiChannels": channels,
        "controlChangeCount": sum(int(track["controlChangeCount"]) for track in tracks),
        "channelPressureCount": sum(int(track["channelPressureCount"]) for track in tracks),
        "sha256": hashlib.sha256(artifact.data).hexdigest(),
    }


def validate_pack_contract(artifacts: tuple[Artifact, ...]) -> None:
    by_path = {artifact.path: inspect_midi(artifact.data) for artifact in artifacts}
    if len(by_path) != len(artifacts):
        raise ValueError("Duplicate melodic pack artifact path")

    chromatic_track = by_path["replication/01-chromatic-range.mid"]["tracks"][0]
    chromatic_notes = [event[2] for event in chromatic_track["notes"]]
    if chromatic_notes != list(range(CAPTURE_LOW, CAPTURE_HIGH + 1)):
        raise ValueError("Chromatic capture does not contain every configured note in order")

    velocity_track = by_path["replication/02-velocity-response.mid"]["tracks"][0]
    velocity_notes = velocity_track["notes"]
    expected_velocity_pairs = [
        (midi, velocity)
        for midi in (36, 60, 81)
        for velocity in VELOCITIES
        for _repeat in range(4)
    ]
    actual_velocity_pairs = [(event[2], event[3]) for event in velocity_notes]
    if actual_velocity_pairs != expected_velocity_pairs:
        raise ValueError("Velocity response schedule differs from the capture contract")

    role_contracts = {
        "01-chords": (1, 36, 72),
        "02-melody": (2, 54, 81),
        "03-bass": (3, 28, 55),
        "04-support": (4, 48, 77),
        "05-drums": (10, 0, 127),
    }
    stem_note_lists: list[list[tuple[int, int, int, int]]] = []
    for filename, (channel, minimum, maximum) in role_contracts.items():
        track = by_path[f"musical-context/{filename}.mid"]["tracks"][0]
        if track["endTick"] != beat(64):
            raise ValueError(f"{filename}: context stem is not exactly 16 bars")
        notes = track["notes"]
        if not notes:
            raise ValueError(f"{filename}: context stem is empty")
        if {event[1] + 1 for event in notes} != {channel}:
            raise ValueError(f"{filename}: unexpected MIDI channel")
        if min(event[2] for event in notes) < minimum or max(event[2] for event in notes) > maximum:
            raise ValueError(f"{filename}: notes exceed the Jam2 role range")
        stem_note_lists.append(notes)

    full = by_path["musical-context/06-full-arrangement.mid"]
    if full["format"] != 1 or full["trackCount"] != 6:
        raise ValueError("Full arrangement must be type 1 with one conductor and five role tracks")
    full_tracks = full["tracks"]
    if any(track["endTick"] != beat(64) for track in full_tracks):
        raise ValueError("Full arrangement tracks are not exactly aligned")
    for index, stem_notes in enumerate(stem_note_lists, start=1):
        if full_tracks[index]["notes"] != stem_notes:
            raise ValueError("Full arrangement differs from an aligned context stem")


def build_pack() -> tuple[tuple[Artifact, ...], dict[str, object]]:
    replication = (
        chromatic_sequence(),
        velocity_sequence(),
        duration_sequence(),
        repeated_sequence(),
        polyphony_sequence(),
        legato_sequence(),
        expression_sequence(),
    )
    complete = combined_capture(replication)
    context = (context_chords(), context_melody(), context_bass(), context_support(), context_drums())
    artifacts: list[Artifact] = [
        Artifact(f"replication/{complete.slug}.mid", "replication-complete", complete.purpose, format_zero(complete))
    ]
    artifacts.extend(
        Artifact(f"replication/{sequence.slug}.mid", "replication-module", sequence.purpose, format_zero(sequence))
        for sequence in replication
    )
    context_filenames = ("01-chords", "02-melody", "03-bass", "04-support", "05-drums")
    artifacts.extend(
        Artifact(f"musical-context/{filename}.mid", "musical-context-stem", sequence.purpose, format_zero(sequence))
        for filename, sequence in zip(context_filenames, context)
    )
    artifacts.append(
        Artifact(
            "musical-context/06-full-arrangement.mid",
            "musical-context-full",
            "The five aligned role tracks in one type-1 MIDI file",
            format_one("Jam2 full melodic context v1", "Five aligned role tracks; 16 bars in A minor", context, beat(64)),
        )
    )
    artifact_tuple = tuple(artifacts)
    validate_pack_contract(artifact_tuple)
    manifest = {
        "version": "melodic-v1",
        "generator": "experiments/synth-ab/tools/generate_melodic_capture_midi.py",
        "tempoBpm": TEMPO_BPM,
        "timeSignature": "4/4",
        "ppq": PPQ,
        "captureRange": {
            "minimum": CAPTURE_LOW,
            "maximum": CAPTURE_HIGH,
            "scientific": f"{note_name(CAPTURE_LOW)}-{note_name(CAPTURE_HIGH)}",
            "ableton": f"{note_name(CAPTURE_LOW, True)}-{note_name(CAPTURE_HIGH, True)}",
        },
        "observedJam2CorpusRanges": {
            "bass": {"minimum": 28, "maximum": 55},
            "chords": {"minimum": 36, "maximum": 72},
            "melody": {"minimum": 54, "maximum": 81},
            "support": {"minimum": 48, "maximum": 77},
        },
        "velocitySteps": list(VELOCITIES),
        "context": {
            "key": "A minor / C major",
            "bars": 16,
            "durationSeconds": seconds_for_ticks(beat(64)),
            "roleChannels": {"chords": 1, "melody": 2, "bass": 3, "support": 4, "drums": 10},
            "drumMap": "General MIDI",
        },
        "files": [artifact_manifest(artifact) for artifact in artifact_tuple],
    }
    return artifact_tuple, manifest


def default_output() -> Path:
    return Path(__file__).resolve().parents[1] / "capture-midi" / "melodic-v1"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate checked-in files without writing")
    parser.add_argument("--output", type=Path, default=default_output(), help="output directory")
    args = parser.parse_args()
    artifacts, manifest = build_pack()
    expected_manifest = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
    output = args.output.resolve()

    if args.check:
        failures: list[str] = []
        for artifact in artifacts:
            path = output / artifact.path
            if not path.is_file():
                failures.append(f"missing {artifact.path}")
            elif path.read_bytes() != artifact.data:
                failures.append(f"changed {artifact.path}")
        manifest_path = output / "manifest.json"
        if not manifest_path.is_file():
            failures.append("missing manifest.json")
        elif manifest_path.read_text(encoding="utf-8") != expected_manifest:
            failures.append("changed manifest.json")
        if failures:
            for failure in failures:
                print(f"FAIL: {failure}")
            return 1
        print(f"Validated {len(artifacts)} MIDI files and manifest in {output}")
        return 0

    for artifact in artifacts:
        path = output / artifact.path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(artifact.data)
    (output / "manifest.json").write_text(expected_manifest, encoding="utf-8")
    print(f"Generated {len(artifacts)} MIDI files and manifest in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
