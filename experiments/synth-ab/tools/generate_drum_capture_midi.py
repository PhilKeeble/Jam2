#!/usr/bin/env python3
"""Generate and validate the Jam2 standardized drum reference MIDI pack.

This uses only the Python standard library so the checked-in MIDI files can be
reproduced without adding a project dependency.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path


PPQ = 480
TEMPO_US_PER_QUARTER = 500_000  # 120 BPM
CHANNEL = 9  # MIDI channel 10, the General MIDI percussion channel.
NOTE_LENGTH_TICKS = PPQ // 4
PRE_ROLL_TICKS = PPQ * 4  # Two seconds at 120 BPM.
VELOCITY_LADDER = (1, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127)
REPEAT_SECTIONS = (
    ("ghost", 24, 12),
    ("soft", 48, 12),
    ("medium", 80, 12),
    ("hard", 112, 12),
)


@dataclass(frozen=True)
class CaptureSpec:
    slug: str
    display_name: str
    note: int
    note_name: str
    tail_class: str
    interval_beats: int
    intended_use: str


CAPTURES = (
    CaptureSpec("kick", "Kick", 36, "C1", "short", 2, "Acoustic and electronic"),
    CaptureSpec("snare", "Snare", 38, "D1", "short", 2, "Acoustic and electronic"),
    CaptureSpec("closed-hat", "Closed hat", 42, "F#1", "short", 2, "Acoustic and electronic"),
    CaptureSpec("high-tom", "High tom", 50, "D2", "short", 2, "Acoustic and electronic"),
    CaptureSpec("mid-tom", "Mid tom", 47, "B1", "short", 2, "Acoustic and electronic"),
    CaptureSpec("floor-tom", "Floor tom", 43, "G1", "short", 2, "Acoustic and electronic"),
    CaptureSpec("cross-stick", "Cross-stick / rim", 37, "C#1", "short", 2, "Primarily acoustic"),
    CaptureSpec("clap", "Clap", 39, "D#1", "short", 2, "Primarily electronic"),
    CaptureSpec("open-hat", "Open hat", 46, "A#1", "long", 8, "Acoustic and electronic"),
    CaptureSpec("crash", "Crash", 49, "C#2", "long", 8, "Acoustic and electronic"),
    CaptureSpec("ride-tip", "Ride tip", 51, "D#2", "long", 8, "Normal and ghost ride hits"),
    CaptureSpec("ride-cup", "Ride cup / bell", 53, "F2", "long", 8, "Ride accents"),
)


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


def encode_track(events: list[tuple[int, int, bytes]], end_tick: int) -> bytes:
    events.append((end_tick, 99, meta_event(0x2F, b"")))
    events.sort(key=lambda event: (event[0], event[1]))
    previous_tick = 0
    encoded = bytearray()
    for tick, _order, payload in events:
        encoded.extend(variable_length(tick - previous_tick))
        encoded.extend(payload)
        previous_tick = tick
    return bytes(encoded)


def midi_file(track: bytes) -> bytes:
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    return header + b"MTrk" + struct.pack(">I", len(track)) + track


def base_events(track_name: str, description: str) -> list[tuple[int, int, bytes]]:
    return [
        (0, 0, meta_event(0x03, track_name.encode("ascii"))),
        (0, 1, meta_event(0x01, description.encode("ascii"))),
        (0, 2, meta_event(0x51, TEMPO_US_PER_QUARTER.to_bytes(3, "big"))),
        (0, 3, meta_event(0x58, bytes((4, 2, 24, 8)))),
    ]


def capture_velocities() -> tuple[int, ...]:
    repeated = tuple(
        velocity
        for _name, velocity, count in REPEAT_SECTIONS
        for _ in range(count)
    )
    return VELOCITY_LADDER + repeated


def make_capture(spec: CaptureSpec) -> tuple[bytes, int]:
    interval_ticks = spec.interval_beats * PPQ
    events = base_events(
        f"Jam2 {spec.display_name} capture v1",
        f"GM {spec.note} {spec.note_name}; 2s pre-roll; {spec.tail_class} tail spacing",
    )
    current_tick = PRE_ROLL_TICKS
    sections = (("17-step velocity ladder", VELOCITY_LADDER),) + tuple(
        (f"12 {name} repeats at velocity {velocity}", (velocity,) * count)
        for name, velocity, count in REPEAT_SECTIONS
    )
    for section_name, velocities in sections:
        events.append((current_tick, 10, meta_event(0x06, section_name.encode("ascii"))))
        for velocity in velocities:
            events.append((current_tick, 20, bytes((0x90 | CHANNEL, spec.note, velocity))))
            events.append((current_tick + NOTE_LENGTH_TICKS, 30, bytes((0x80 | CHANNEL, spec.note, 0))))
            current_tick += interval_ticks
    return midi_file(encode_track(events, current_tick)), current_tick


def make_map_check() -> tuple[bytes, int]:
    events = base_events(
        "Jam2 kit map check v1",
        "One medium hit per standardized drum articulation; setup aid only",
    )
    current_tick = PRE_ROLL_TICKS
    for spec in CAPTURES:
        events.append((current_tick, 10, meta_event(0x06, spec.display_name.encode("ascii"))))
        events.append((current_tick, 20, bytes((0x90 | CHANNEL, spec.note, 80))))
        events.append((current_tick + NOTE_LENGTH_TICKS, 30, bytes((0x80 | CHANNEL, spec.note, 0))))
        current_tick += PPQ
    return midi_file(encode_track(events, current_tick)), current_tick


def read_variable_length(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for _ in range(4):
        byte = data[offset]
        offset += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, offset
    raise ValueError("Invalid MIDI variable-length integer")


def inspect_midi(data: bytes) -> dict[str, object]:
    if len(data) < 22 or data[:4] != b"MThd":
        raise ValueError("Missing MIDI header")
    header_length, file_format, track_count, division = struct.unpack(">IHHH", data[4:14])
    if header_length != 6 or file_format != 0 or track_count != 1 or division != PPQ:
        raise ValueError("Unexpected MIDI header fields")
    if data[14:18] != b"MTrk":
        raise ValueError("Missing MIDI track chunk")
    track_length = struct.unpack(">I", data[18:22])[0]
    track = data[22 : 22 + track_length]
    if len(track) != track_length or 22 + track_length != len(data):
        raise ValueError("Invalid MIDI track length")

    offset = 0
    tick = 0
    running_status: int | None = None
    note_ons: list[tuple[int, int, int, int]] = []
    note_offs = 0
    end_tick: int | None = None
    while offset < len(track):
        delta, offset = read_variable_length(track, offset)
        tick += delta
        status = track[offset]
        if status < 0x80:
            if running_status is None:
                raise ValueError("Running status used before a channel status")
            status = running_status
        else:
            offset += 1

        if status == 0xFF:
            running_status = None
            kind = track[offset]
            offset += 1
            length, offset = read_variable_length(track, offset)
            offset += length
            if kind == 0x2F:
                end_tick = tick
                break
        elif status in (0xF0, 0xF7):
            running_status = None
            length, offset = read_variable_length(track, offset)
            offset += length
        else:
            running_status = status
            command = status & 0xF0
            channel = status & 0x0F
            data_length = 1 if command in (0xC0, 0xD0) else 2
            payload = track[offset : offset + data_length]
            offset += data_length
            if command == 0x90 and payload[1] != 0:
                note_ons.append((tick, channel, payload[0], payload[1]))
            elif command == 0x80 or (command == 0x90 and payload[1] == 0):
                note_offs += 1

    if end_tick is None:
        raise ValueError("MIDI track has no end-of-track event")
    return {"note_ons": note_ons, "note_offs": note_offs, "end_tick": end_tick}


def seconds_for_ticks(ticks: int) -> float:
    return ticks * TEMPO_US_PER_QUARTER / PPQ / 1_000_000


def capture_filename(index: int, spec: CaptureSpec) -> str:
    return f"{index:02d}-{spec.slug}-gm{spec.note}.mid"


def validate_capture(path: Path, spec: CaptureSpec, expected_end_tick: int) -> dict[str, object]:
    data = path.read_bytes()
    inspection = inspect_midi(data)
    note_ons = inspection["note_ons"]
    assert isinstance(note_ons, list)
    expected_velocities = capture_velocities()
    actual_velocities = tuple(event[3] for event in note_ons)
    actual_notes = {event[2] for event in note_ons}
    actual_channels = {event[1] for event in note_ons}
    if actual_velocities != expected_velocities:
        raise ValueError(f"{path.name}: velocity schedule differs from the contract")
    if actual_notes != {spec.note} or actual_channels != {CHANNEL}:
        raise ValueError(f"{path.name}: note or MIDI channel differs from the contract")
    if inspection["note_offs"] != len(expected_velocities):
        raise ValueError(f"{path.name}: note-on/note-off count mismatch")
    if inspection["end_tick"] != expected_end_tick:
        raise ValueError(f"{path.name}: duration differs from the contract")
    return {
        "file": path.name,
        "displayName": spec.display_name,
        "midiNote": spec.note,
        "abletonNoteName": spec.note_name,
        "midiChannel": CHANNEL + 1,
        "tailClass": spec.tail_class,
        "intervalSeconds": seconds_for_ticks(spec.interval_beats * PPQ),
        "durationSeconds": seconds_for_ticks(expected_end_tick),
        "hitCount": len(expected_velocities),
        "intendedUse": spec.intended_use,
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def validate_map_check(path: Path, expected_end_tick: int) -> dict[str, object]:
    data = path.read_bytes()
    inspection = inspect_midi(data)
    note_ons = inspection["note_ons"]
    assert isinstance(note_ons, list)
    if tuple(event[2] for event in note_ons) != tuple(spec.note for spec in CAPTURES):
        raise ValueError("Map-check notes differ from the capture definitions")
    if any(event[1] != CHANNEL or event[3] != 80 for event in note_ons):
        raise ValueError("Map-check channel or velocity differs from the contract")
    if inspection["note_offs"] != len(CAPTURES) or inspection["end_tick"] != expected_end_tick:
        raise ValueError("Map-check event count or duration differs from the contract")
    return {
        "file": path.name,
        "durationSeconds": seconds_for_ticks(expected_end_tick),
        "hitCount": len(CAPTURES),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def expected_end_ticks() -> dict[str, int]:
    hit_count = len(capture_velocities())
    return {
        capture_filename(index, spec): PRE_ROLL_TICKS + hit_count * spec.interval_beats * PPQ
        for index, spec in enumerate(CAPTURES, start=1)
    }


def write_pack(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    expected_files = {"00-kit-map-check.mid"} | {
        capture_filename(index, spec)
        for index, spec in enumerate(CAPTURES, start=1)
    }
    for path in output.glob("*.mid"):
        if path.name not in expected_files:
            path.unlink()
    map_data, _map_end = make_map_check()
    (output / "00-kit-map-check.mid").write_bytes(map_data)
    for index, spec in enumerate(CAPTURES, start=1):
        data, _end_tick = make_capture(spec)
        (output / capture_filename(index, spec)).write_bytes(data)


def validate_pack(output: Path, write_manifest: bool) -> dict[str, object]:
    end_ticks = expected_end_ticks()
    map_end_tick = PRE_ROLL_TICKS + len(CAPTURES) * PPQ
    map_summary = validate_map_check(output / "00-kit-map-check.mid", map_end_tick)
    capture_summaries = [
        validate_capture(output / capture_filename(index, spec), spec, end_ticks[capture_filename(index, spec)])
        for index, spec in enumerate(CAPTURES, start=1)
    ]
    manifest = {
        "schema": "jam2-drum-reference-capture-v1",
        "tempoBpm": 120,
        "ticksPerQuarter": PPQ,
        "preRollSeconds": seconds_for_ticks(PRE_ROLL_TICKS),
        "noteLengthSeconds": seconds_for_ticks(NOTE_LENGTH_TICKS),
        "velocityLadder": list(VELOCITY_LADDER),
        "repeatSections": [
            {"name": name, "velocity": velocity, "hitCount": count}
            for name, velocity, count in REPEAT_SECTIONS
        ],
        "mapCheck": map_summary,
        "captures": capture_summaries,
    }
    manifest_path = output / "manifest.json"
    serialized = json.dumps(manifest, indent=2) + "\n"
    if write_manifest:
        manifest_path.write_text(serialized, encoding="utf-8")
    elif not manifest_path.exists() or manifest_path.read_text(encoding="utf-8") != serialized:
        raise ValueError("manifest.json is missing or does not match the MIDI files")
    return manifest


def main() -> int:
    default_output = Path(__file__).resolve().parents[1] / "capture-midi" / "drums-v1"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--check", action="store_true", help="validate the existing pack without rewriting it")
    args = parser.parse_args()

    output = args.output.resolve()
    if not args.check:
        write_pack(output)
    manifest = validate_pack(output, write_manifest=not args.check)
    action = "Validated" if args.check else "Generated and validated"
    print(f"{action} {len(manifest['captures'])} capture files in {output}")
    for capture in manifest["captures"]:
        minutes, seconds = divmod(capture["durationSeconds"], 60)
        print(
            f"  {capture['file']}: {capture['hitCount']} hits, "
            f"{int(minutes)}:{seconds:04.1f}, SHA256 {capture['sha256'][:12]}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
