#!/usr/bin/env python3
"""Export the exact eight-bar Instrument Lab profile auditions as MIDI.

The native sound lab remains the performance source of truth. Its seed audit
contains the same stable-seed, complexity-4 events used by the web app's
"Generated style phrase" audition. This script packages those events into
Ableton-friendly stem and type-1 arrangement files, grouped by style/profile.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path


PPQ = 480
ROLE_ORDER = ("chords", "melody", "bass", "support", "drums")
ROLE_CHANNELS = {"chords": 0, "melody": 1, "bass": 2, "support": 3, "drums": 9}
ROLE_FILENAMES = {
    "chords": "01-chords.mid",
    "melody": "02-melody.mid",
    "bass": "03-bass.mid",
    "support": "04-support.mid",
    "drums": "05-drums.mid",
}
ROLE_NAMES = {
    "chords": "Chords / comping",
    "melody": "Melody / lead",
    "bass": "Bass",
    "support": "Supporting line",
    "drums": "Drums / percussion",
}
MANIFEST_PREFIX = "window.JAM2_SOUND_DESIGN_MANIFEST = "


MidiEvent = tuple[int, int, bytes]


@dataclass(frozen=True)
class Artifact:
    path: str
    role: str
    data: bytes
    note_count: int
    minimum_note: int | None
    maximum_note: int | None


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


def text_meta(kind: int, value: str) -> bytes:
    return meta_event(kind, value.encode("utf-8"))


def track_chunk(events: list[MidiEvent], end_tick: int) -> bytes:
    ordered = list(events)
    ordered.append((end_tick, 100, meta_event(0x2F, b"")))
    ordered.sort(key=lambda item: (item[0], item[1], item[2]))
    previous = 0
    body = bytearray()
    for tick, _order, payload in ordered:
        if tick < previous or tick > end_tick:
            raise ValueError(f"MIDI event at {tick} lies outside 0..{end_tick}")
        body.extend(variable_length(tick - previous))
        body.extend(payload)
        previous = tick
    return b"MTrk" + struct.pack(">I", len(body)) + body


def midi_file(tracks: list[list[MidiEvent]], end_tick: int, midi_format: int) -> bytes:
    header = b"MThd" + struct.pack(">IHHH", 6, midi_format, len(tracks), PPQ)
    return header + b"".join(track_chunk(track, end_tick) for track in tracks)


def conductor_events(
    profile_name: str,
    profile_id: str,
    seed: str,
    quarter_bpm: float,
    numerator: int,
    denominator: int,
    tonic: str,
    mode: str,
) -> list[MidiEvent]:
    if denominator <= 0 or denominator & (denominator - 1):
        raise ValueError(f"Unsupported MIDI time-signature denominator: {denominator}")
    tempo = round(60_000_000 / quarter_bpm)
    denominator_power = int(math.log2(denominator))
    return [
        (0, 0, text_meta(0x03, profile_name)),
        (0, 1, text_meta(0x01, f"Jam2 profile {profile_id}; exact web audition seed {seed}")),
        (0, 2, meta_event(0x51, tempo.to_bytes(3, "big"))),
        (0, 3, meta_event(0x58, bytes((numerator, denominator_power, 24, 8)))),
        (0, 4, text_meta(0x06, f"{tonic} {mode}")),
    ]


def role_track_events(role: str, profile_name: str, notes: list[dict[str, int]]) -> list[MidiEvent]:
    events: list[MidiEvent] = [
        (0, 0, text_meta(0x03, f"{profile_name} - {ROLE_NAMES[role]}")),
        (0, 1, text_meta(0x01, "Timing and velocity match the deterministic Instrument Lab profile audition.")),
    ]
    channel = ROLE_CHANNELS[role]
    for note in notes:
        start = note["start"]
        end = note["end"]
        midi = note["midi"]
        velocity = note["velocity"]
        events.append((start, 30, bytes((0x90 | channel, midi, velocity))))
        events.append((end, 20, bytes((0x80 | channel, midi, 0))))
    return events


def load_sound_manifest(path: Path) -> dict:
    raw = path.read_text(encoding="utf-8")
    if not raw.startswith(MANIFEST_PREFIX):
        raise ValueError(f"Unexpected sound-design manifest wrapper: {path}")
    payload = raw[len(MANIFEST_PREFIX):].strip()
    if payload.endswith(";"):
        payload = payload[:-1]
    return json.loads(payload)


def source_beat_ticks(recipe: dict) -> int:
    beat_unit = int(recipe["time"]["beat_unit"])
    ticks = PPQ * 4
    if ticks % beat_unit:
        raise ValueError(f"Beat unit {beat_unit} cannot be represented at {PPQ} PPQ")
    return ticks // beat_unit


def tick_at(value: float, beat_ticks: int, end_tick: int) -> int:
    return max(0, min(end_tick, round(float(value) * beat_ticks)))


def melodic_notes(
    audit: dict,
    role: str,
    beat_ticks: int,
    end_tick: int,
    bpm: float,
) -> tuple[list[dict[str, int]], bool]:
    notes: list[dict[str, int]] = []
    for event in audit["audition_lane_events"]:
        if event["role"] != role:
            continue
        start = tick_at(event["start_beat"], beat_ticks, end_tick)
        duration = max(1, round(float(event["duration_beats"]) * beat_ticks))
        notes.append({
            "start": start,
            "end": min(end_tick, start + duration),
            "midi": int(event["midi"]),
            "velocity": int(event["velocity"]),
        })
    if notes:
        return notes, False

    # Instrument Lab deliberately supplies this diagnostic note when a role is
    # exposed by a profile but its generated performance has no active lane.
    seconds_per_source_beat = 60.0 / bpm
    start = tick_at(0.20 / seconds_per_source_beat, beat_ticks, end_tick)
    duration = max(1, round((1.40 / seconds_per_source_beat) * beat_ticks))
    return [{"start": start, "end": min(end_tick, start + duration), "midi": 60, "velocity": 92}], True


def drum_notes(audit: dict, beat_ticks: int, end_tick: int) -> list[dict[str, int]]:
    if "audition_drum_events" not in audit:
        raise ValueError(
            f"{audit['id']}: seed audit lacks audition_drum_events; rebuild the native audit first"
        )
    starts: list[tuple[int, dict]] = []
    for event in audit["audition_drum_events"]:
        starts.append((tick_at(event["start_beat"], beat_ticks, end_tick), event))
    starts.sort(key=lambda item: (item[0], int(item[1]["midi"])))
    notes: list[dict[str, int]] = []
    trigger_length = max(1, PPQ // 8)
    for index, (start, event) in enumerate(starts):
        following = end_tick
        midi = int(event["midi"])
        for next_start, next_event in starts[index + 1:]:
            if int(next_event["midi"]) == midi:
                following = next_start
                break
        end = min(end_tick, start + trigger_length)
        if following > start:
            end = min(end, following)
        notes.append({
            "start": start,
            "end": max(start + 1, end),
            "midi": midi,
            "velocity": int(event["velocity"]),
        })
    return notes


def note_range(notes: list[dict[str, int]]) -> tuple[int | None, int | None]:
    values = [note["midi"] for note in notes]
    return (min(values), max(values)) if values else (None, None)


def build_profile(
    style: dict,
    profile: dict,
    audit: dict,
) -> tuple[list[Artifact], dict]:
    recipe = audit["recipe"]
    time = recipe["time"]
    beat_ticks = source_beat_ticks(recipe)
    bars = int(audit["audition_bars"])
    source_beats = bars * int(recipe["beats_per_bar"])
    end_tick = source_beats * beat_ticks
    source_bpm = float(recipe["bpm"])
    beat_unit = int(time["beat_unit"])
    quarter_bpm = source_bpm * 4.0 / beat_unit
    numerator = int(time["numerator"])
    denominator = int(time["denominator"])
    seed = str(audit["seed"])
    base = f"{style['id']}/{profile['id']}"
    conductor = conductor_events(
        profile["name"], profile["id"], seed, quarter_bpm,
        numerator, denominator, recipe["tonic"], recipe["mode"],
    )

    exposed_roles = [item["id"] for item in profile["roles"]]
    roles = [role for role in ROLE_ORDER if role in exposed_roles]
    role_tracks: dict[str, list[MidiEvent]] = {}
    role_notes: dict[str, list[dict[str, int]]] = {}
    fallback_roles: list[str] = []
    artifacts: list[Artifact] = []
    for role in roles:
        if role == "drums":
            notes = drum_notes(audit, beat_ticks, end_tick)
        else:
            notes, fallback = melodic_notes(audit, role, beat_ticks, end_tick, source_bpm)
            if fallback:
                fallback_roles.append(role)
        events = role_track_events(role, profile["name"], notes)
        role_tracks[role] = events
        role_notes[role] = notes
        data = midi_file([conductor + events], end_tick, 0)
        low, high = note_range(notes)
        artifacts.append(Artifact(
            f"{base}/{ROLE_FILENAMES[role]}", role, data,
            len(notes), low, high,
        ))

    full_tracks = [conductor] + [role_tracks[role] for role in roles]
    full_data = midi_file(full_tracks, end_tick, 1)
    all_notes = [note for role in roles for note in role_notes[role]]
    low, high = note_range(all_notes)
    artifacts.append(Artifact(
        f"{base}/06-full-arrangement.mid", "full-arrangement", full_data,
        len(all_notes), low, high,
    ))

    seconds = end_tick / PPQ * 60.0 / quarter_bpm
    profile_record = {
        "schema": "jam2-profile-context-v1",
        "styleId": style["id"],
        "styleName": style["name"],
        "profileId": profile["id"],
        "profileName": profile["name"],
        "seed": seed,
        "generatorVersion": recipe["generator_version"],
        "complexity": recipe["complexity"],
        "bars": bars,
        "timeSignature": f"{numerator}/{denominator}",
        "sourcePulseBpm": source_bpm,
        "sourcePulseBeatUnit": beat_unit,
        "dawQuarterNoteBpm": quarter_bpm,
        "durationSeconds": round(seconds, 6),
        "tonic": recipe["tonic"],
        "mode": recipe["mode"],
        "formId": recipe["native_form"]["id"],
        "grooveId": recipe["groove"]["id"],
        "fingerprints": recipe["fingerprints"],
        "roles": roles,
        "diagnosticFallbackRoles": fallback_roles,
        "source": "experiments/synth-ab/site/seed-audit.json",
        "files": [artifact.path.rsplit("/", 1)[-1] for artifact in artifacts],
    }
    return artifacts, profile_record


def artifact_record(artifact: Artifact, quarter_bpm: float, end_tick: int) -> dict:
    return {
        "file": artifact.path,
        "role": artifact.role,
        "midiFormat": 1 if artifact.role == "full-arrangement" else 0,
        "ppq": PPQ,
        "durationSeconds": round(end_tick / PPQ * 60.0 / quarter_bpm, 6),
        "noteOnCount": artifact.note_count,
        "noteRange": None if artifact.minimum_note is None else {
            "minimum": artifact.minimum_note,
            "maximum": artifact.maximum_note,
        },
        "sha256": hashlib.sha256(artifact.data).hexdigest(),
    }


def build_pack(sound_manifest: dict, seed_audit: dict) -> tuple[list[Artifact], dict[str, str], dict]:
    profiles = {item["id"]: item for item in sound_manifest["profiles"]}
    audits = {item["id"]: item for item in seed_audit["profiles"]}
    if set(profiles) != set(audits):
        raise ValueError("Sound manifest and seed audit profile sets differ")

    artifacts: list[Artifact] = []
    metadata_files: dict[str, str] = {}
    style_records: list[dict] = []
    for style in sound_manifest["styles"]:
        profile_records: list[dict] = []
        for profile_id in style["profileIds"]:
            profile = profiles[profile_id]
            profile_artifacts, record = build_profile(style, profile, audits[profile_id])
            artifacts.extend(profile_artifacts)
            metadata_path = f"{style['id']}/{profile_id}/profile.json"
            metadata_files[metadata_path] = json.dumps(record, indent=2) + "\n"
            recipe = audits[profile_id]["recipe"]
            end_tick = (
                int(audits[profile_id]["audition_bars"])
                * int(recipe["beats_per_bar"])
                * source_beat_ticks(recipe)
            )
            quarter_bpm = float(recipe["bpm"]) * 4.0 / int(recipe["time"]["beat_unit"])
            record["artifacts"] = [
                artifact_record(item, quarter_bpm, end_tick)
                for item in profile_artifacts
            ]
            profile_records.append(record)
        style_records.append({
            "id": style["id"],
            "name": style["name"],
            "profiles": profile_records,
        })

    manifest = {
        "schema": "jam2-profile-context-pack-v1",
        "generator": "experiments/synth-ab/tools/generate_profile_context_midi.py",
        "performanceSource": "the same deterministic eight-bar native profile audition used by the Synth A/B web app",
        "ppq": PPQ,
        "styles": style_records,
    }
    return artifacts, metadata_files, manifest


def default_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main() -> int:
    root = default_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate generated files without writing")
    parser.add_argument("--sound-manifest", type=Path, default=root / "site" / "sound-design-manifest.js")
    parser.add_argument("--seed-audit", type=Path, default=root / "site" / "seed-audit.json")
    parser.add_argument("--output", type=Path, default=root / "capture-midi" / "melodic-v1" / "musical-context")
    args = parser.parse_args()

    sound_manifest = load_sound_manifest(args.sound_manifest.resolve())
    seed_audit = json.loads(args.seed_audit.resolve().read_text(encoding="utf-8"))
    artifacts, metadata_files, manifest = build_pack(sound_manifest, seed_audit)
    manifest_text = json.dumps(manifest, indent=2) + "\n"
    output = args.output.resolve()

    expected_binary = {artifact.path: artifact.data for artifact in artifacts}
    expected_text = dict(metadata_files)
    expected_text["profile-context-manifest.json"] = manifest_text
    if args.check:
        failures: list[str] = []
        for relative, data in expected_binary.items():
            path = output / relative
            if not path.is_file():
                failures.append(f"missing {relative}")
            elif path.read_bytes() != data:
                failures.append(f"changed {relative}")
        for relative, value in expected_text.items():
            path = output / relative
            if not path.is_file():
                failures.append(f"missing {relative}")
            elif path.read_text(encoding="utf-8") != value:
                failures.append(f"changed {relative}")
        if failures:
            for failure in failures:
                print(f"FAIL: {failure}")
            return 1
        print(
            f"Validated {len(artifacts)} MIDI files across "
            f"{len(manifest['styles'])} styles and {len(sound_manifest['profiles'])} profiles"
        )
        return 0

    for relative, data in expected_binary.items():
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for relative, value in expected_text.items():
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding="utf-8")
    print(
        f"Generated {len(artifacts)} MIDI files across "
        f"{len(manifest['styles'])} styles and {len(sound_manifest['profiles'])} profiles in {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
