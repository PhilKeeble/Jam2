#!/usr/bin/env python3
"""Report the MIDI notes used by named tracks and clips in an Ableton Live Set."""

from __future__ import annotations

import argparse
import gzip
import json
import xml.etree.ElementTree as ET
from pathlib import Path


def value_at(element: ET.Element, path: str, default: str = "") -> str:
    node = element.find(path)
    return node.attrib.get("Value", default) if node is not None else default


def inspect_live_set(path: Path) -> dict[str, object]:
    with gzip.open(path, "rb") as source:
        root = ET.parse(source).getroot()

    tracks_node = root.find("./LiveSet/Tracks")
    if tracks_node is None:
        raise ValueError(f"{path} does not contain LiveSet/Tracks")

    tracks: list[dict[str, object]] = []
    for track in tracks_node:
        track_name = value_at(track, "./Name/EffectiveName") or value_at(track, "./Name/UserName")
        clips: list[dict[str, object]] = []
        notes_seen: set[int] = set()
        for clip in track.iter("MidiClip"):
            keys: list[dict[str, int]] = []
            for key_track in clip.iter("KeyTrack"):
                midi_key = key_track.find("./MidiKey")
                if midi_key is None or "Value" not in midi_key.attrib:
                    continue
                note = int(midi_key.attrib["Value"])
                event_count = sum(1 for _event in key_track.iter("MidiNoteEvent"))
                keys.append({"note": note, "eventCount": event_count})
                notes_seen.add(note)
            clips.append(
                {
                    "name": value_at(clip, "./Name"),
                    "currentStartBeats": float(value_at(clip, "./CurrentStart", "0")),
                    "currentEndBeats": float(value_at(clip, "./CurrentEnd", "0")),
                    "keys": keys,
                }
            )
        tracks.append(
            {
                "id": int(track.attrib.get("Id", "-1")),
                "type": track.tag,
                "name": track_name,
                "notes": sorted(notes_seen),
                "clips": clips,
            }
        )
    return {"schema": "jam2-ableton-drum-map-inspection-v1", "source": str(path), "tracks": tracks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("live_set", type=Path, help="Ableton .als file")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    report = inspect_live_set(args.live_set.resolve())
    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    for track in report["tracks"]:
        notes = ", ".join(str(note) for note in track["notes"]) or "none"
        print(f"{track['name']}: MIDI notes {notes}")
        for clip in track["clips"]:
            key_summary = ", ".join(
                f"{key['note']} ({key['eventCount']} events)" for key in clip["keys"]
            ) or "no note events"
            print(f"  {clip['name']}: {key_summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

