#!/usr/bin/env python3
"""Render and measure the non-FM AbletonCorePiano reference match."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from compare_ableton_pop_renders import metrics, read_wav, slice_seconds
from generate_melodic_capture_midi import (
    PPQ,
    chromatic_sequence,
    context_chords,
    velocity_sequence,
)


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
SECONDS_PER_TICK = 0.5 / PPQ

PATCH = {
    "source": "additive-harmonic",
    "secondSource": "off",
    "sourceBlend": 0,
    "harmonicFamily": 6,
    "shape": .3,
    "width": .58,
    "oscillator2Mix": .34,
    "detuneCents": 5.5,
    "attackSeconds": .001,
    "decaySeconds": .5,
    "sustain": .07,
    "releaseSeconds": .14,
    "velocitySensitivity": 1,
    "filterArchitecture": "state-variable-lowpass",
    "filterCutoffHz": 1000,
    "filterEnvelopeHz": 0,
    "filterKeyTracking": 0,
    "resonance": .05,
    "filterDrive": 1.35,
    "voiceDrive": 1,
    "busDrive": 1,
    "chorusMix": .14,
    "chorusDepth": .18,
    "chorusRateHz": .32,
    "stereoSpread": .25,
    "stereoWidth": 2,
}


def note_events(sequence) -> list[dict]:
    active: dict[tuple[int, int], list[tuple[int, int]]] = {}
    notes: list[dict] = []
    for tick, _order, payload in sorted(
        sequence.events, key=lambda item: (item[0], item[1])
    ):
        if len(payload) != 3:
            continue
        command = payload[0] & 0xF0
        key = (payload[0] & 0x0F, payload[1])
        if command == 0x90 and payload[2] > 0:
            active.setdefault(key, []).append((tick, payload[2]))
        elif command in (0x80, 0x90) and active.get(key):
            start, velocity = active[key].pop(0)
            notes.append({
                "startSeconds": start * SECONDS_PER_TICK,
                "durationSeconds": max(1, tick - start) * SECONDS_PER_TICK,
                "midi": key[1],
                "velocity": velocity,
            })
    if any(active.values()):
        raise ValueError(f"Unclosed notes in {sequence.name}")
    return sorted(notes, key=lambda item: (item["startSeconds"], item["midi"]))


def request(events: list[dict], duration: float) -> dict:
    return {
        "schema": "jam2-instrument-patch-v1",
        "profileId": "pop_loop",
        "role": "chords",
        "audition": "reference-events",
        "patch": PATCH,
        "referenceEvents": events,
        "referenceDurationSeconds": duration,
    }


def render(executable: Path, request_path: Path, output_path: Path, body: dict) -> dict:
    request_path.write_text(
        json.dumps(body, separators=(",", ":")) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment["PATH"] = str(REPO / "release") + os.pathsep + environment.get("PATH", "")
    completed = subprocess.run(
        [str(executable), "--render-instrument", str(request_path), str(output_path)],
        cwd=REPO,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    result = json.loads([line for line in completed.stdout.splitlines() if line.strip()][-1])
    if not result.get("ok"):
        raise RuntimeError(str(result))
    return result


def comparison(reference_dir: Path, output: Path) -> dict:
    full_rate, full = read_wav(
        reference_dir / "Piano Refernce Chord - Piano Refernce.wav"
    )
    context_rate, context = read_wav(reference_dir / "Piano Chord - Piano.wav")
    chromatic_rate, chromatic = read_wav(output / "chromatic.wav")
    velocity_rate, velocity = read_wav(output / "velocity.wav")
    candidate_context_rate, candidate_context = read_wav(output / "context.wav")

    selected_notes = {}
    for midi in (36, 48, 60, 72, 81):
        start = 2 + (midi - 28) * 2
        selected_notes[str(midi)] = {
            "reference": metrics(slice_seconds(full, full_rate, start, 1.9), full_rate),
            "candidate": metrics(
                slice_seconds(chromatic, chromatic_rate, start, 1.9), chromatic_rate
            ),
        }

    velocity_offset = chromatic_sequence().end_tick * SECONDS_PER_TICK + 4
    c4_events = [event for event in note_events(velocity_sequence()) if event["midi"] == 60]
    selected_velocities = {}
    for target in (20, 50, 80, 110, 127):
        event = next(item for item in c4_events if item["velocity"] == target)
        start = event["startSeconds"]
        selected_velocities[str(target)] = {
            "reference": metrics(
                slice_seconds(full, full_rate, velocity_offset + start, 1.9), full_rate
            ),
            "candidate": metrics(
                slice_seconds(velocity, velocity_rate, start, 1.9), velocity_rate
            ),
        }

    return {
        "selectedChromaticNotes": selected_notes,
        "middleCVelocity": selected_velocities,
        "context": {
            "reference": metrics(context, context_rate, True),
            "candidate": metrics(candidate_context, candidate_context_rate, True),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable", type=Path, default=ROOT / "build" / "jam2_sound_lab.exe"
    )
    parser.add_argument(
        "--output", type=Path, default=ROOT / "artifacts" / "piano-compare" / "ableton-core-piano"
    )
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()

    output = args.output.resolve()
    requests = output / "requests"
    requests.mkdir(parents=True, exist_ok=True)
    sequences = {
        "chromatic": chromatic_sequence(),
        "velocity": velocity_sequence(),
        "context": context_chords(),
    }
    report: dict[str, object] = {
        "schema": "jam2-ableton-core-piano-comparison-v1",
        "design": "key-dependent additive piano; no FM",
        "patch": PATCH,
        "renders": {},
    }
    for name, sequence in sequences.items():
        duration = sequence.end_tick * SECONDS_PER_TICK
        if name == "context":
            duration += 4
        report["renders"][name] = render(
            args.executable.resolve(),
            requests / f"{name}.json",
            output / f"{name}.wav",
            request(note_events(sequence), duration),
        )
        print(f"rendered {name}", flush=True)

    if args.reference_dir:
        report["comparison"] = comparison(args.reference_dir.resolve(), output)
    report_path = output / "comparison-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {report_path}")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
