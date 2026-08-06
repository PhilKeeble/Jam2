#!/usr/bin/env python3
"""Render Ableton808 captures and exact reference patterns through the lab."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

from analyze_drum_reference_captures import CAPTURES, FIRST_HIT_SECONDS, VELOCITIES
from generate_808_pattern_midi import PATTERNS, PIECES, suite_offsets, suite_total_beats


TICKS_PER_BEAT = 12
PROFILE_ID = "electronic_house"


def render(executable: Path, request_path: Path, output_path: Path, request: dict) -> dict:
    request_path.write_text(json.dumps(request, separators=(",", ":")) + "\n", encoding="utf-8")
    environment = os.environ.copy()
    release = executable.resolve().parents[3] / "release"
    environment["PATH"] = str(release) + os.pathsep + environment.get("PATH", "")
    completed = subprocess.run(
        [str(executable), "--render-instrument", str(request_path), str(output_path)],
        cwd=executable.resolve().parents[3],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"Render failed for {output_path.name}: {completed.stderr.strip() or completed.stdout.strip()}"
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(f"Render returned no JSON for {output_path.name}")
    result = json.loads(lines[-1])
    if not result.get("ok"):
        raise RuntimeError(f"Render did not report success for {output_path.name}")
    return result


def capture_pattern(piece: str, interval_seconds: float) -> dict:
    ticks_per_second = TICKS_PER_BEAT * 2
    first_tick = round(FIRST_HIT_SECONDS * ticks_per_second)
    interval_ticks = round(interval_seconds * ticks_per_second)
    events = [
        {"piece": piece, "tick": first_tick + index * interval_ticks, "velocity": velocity}
        for index, velocity in enumerate(VELOCITIES)
    ]
    final_window_seconds = FIRST_HIT_SECONDS + len(VELOCITIES) * interval_seconds
    beats = round(final_window_seconds * 2)
    return {"bpm": 120, "beats": beats, "tailSeconds": 4, "events": events}


def suite_pattern() -> dict:
    events = []
    engine_piece_ids = {"clap": "snare"}
    for pattern, beat_offset in zip(PATTERNS, suite_offsets()):
        for piece in PIECES:
            for beat, velocity in pattern.events[piece.slug]:
                events.append(
                    {
                        "piece": engine_piece_ids.get(piece.slug, piece.slug),
                        "tick": int((beat + beat_offset) * TICKS_PER_BEAT),
                        "velocity": velocity,
                    }
                )
    events.sort(key=lambda event: (event["tick"], event["piece"]))
    return {
        "bpm": 120,
        "beats": suite_total_beats(),
        "tailSeconds": 4,
        "events": events,
    }


def request_for(kit: dict, reference_pattern: dict) -> dict:
    return {
        "schema": "jam2-instrument-patch-v1",
        "profileId": PROFILE_ID,
        "role": "drums",
        "audition": "reference-pattern",
        "kit": kit,
        "referencePattern": reference_pattern,
    }


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable",
        type=Path,
        default=root / "build" / "jam2_sound_lab.exe",
    )
    parser.add_argument("--kit", type=Path, required=True, help="serialized normalized or template kit JSON")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--pieces",
        nargs="*",
        choices=[spec[0] for spec in CAPTURES],
        help="render only these isolated pieces",
    )
    parser.add_argument("--skip-suite", action="store_true")
    args = parser.parse_args()

    executable = args.executable.resolve()
    # Accept PowerShell-generated JSON artifacts as well as BOM-free files.
    kit = json.loads(args.kit.read_text(encoding="utf-8-sig"))
    kit["candidateId"] = "base-ableton-808"
    kit["candidateName"] = "Ableton808"
    output = args.output.resolve()
    captures = output / "captures"
    requests = output / "requests"
    captures.mkdir(parents=True, exist_ok=True)
    requests.mkdir(parents=True, exist_ok=True)
    results = []
    selected = set(args.pieces or ())
    captures_to_render = [
        spec for spec in CAPTURES if not selected or spec[0] in selected
    ]
    for piece, filename, interval, _source_sample in captures_to_render:
        result = render(
            executable,
            requests / f"capture-{piece}.json",
            captures / filename,
            request_for(kit, capture_pattern(piece, interval)),
        )
        results.append(
            {
                "piece": piece,
                "output": f"captures/{filename}",
                "events": result["events"],
                "metrics": result["metrics"],
            }
        )
        print(f"capture {piece}: {result['events']} events", flush=True)

    suite_result = None
    if not args.skip_suite:
        suite_result = render(
            executable,
            requests / "comparison-suite.json",
            output / "complete-pattern-suite.wav",
            request_for(kit, suite_pattern()),
        )
        if suite_result.get("patternSource") != "reference-midi-events":
            raise RuntimeError("Comparison suite did not use exact reference MIDI events")
    report = {
        "schema": "jam2-ableton808-render-comparison-v1",
        "kit": kit["candidateId"],
        "captures": results,
        "suite": None if suite_result is None else {
            "output": "complete-pattern-suite.wav",
            "events": suite_result["events"],
            "frames": suite_result["frames"],
            "sampleRate": suite_result["sampleRate"],
            "patternSource": suite_result["patternSource"],
            "metrics": suite_result["metrics"],
        },
    }
    (output / "render-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if suite_result is not None:
        print(f"suite: {suite_result['events']} events, {suite_result['frames'] / suite_result['sampleRate']:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
