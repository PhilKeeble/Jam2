#!/usr/bin/env python3
"""Render the Ableton Pop reference instruments through the Synth A/B engine.

This uses the exact events from the generated context MIDI source of truth so
the 36-second renders align with the Ableton context stems sample-for-sample.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import wave

from generate_melodic_capture_midi import (
    PPQ,
    context_bass,
    context_chords,
    context_drums,
    context_melody,
    context_support,
)


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
SECONDS_PER_TICK = 0.5 / PPQ
DRUM_TICKS_PER_BEAT = 12

PATCHES = {
    "bass": {
        "source": "sine-fundamental", "secondSource": "sine-fundamental",
        "sourceBlend": .04, "secondSourceTranspose": -12,
        "secondSourceDetuneCents": 0,
        "filterArchitecture": "source-direct", "subMix": 0,
        "attackSeconds": .001, "decaySeconds": .12, "sustain": .25,
        "releaseSeconds": .62, "velocitySensitivity": .36,
        "pitchEnvelopeSemitones": 14, "pitchEnvelopeSeconds": .018,
        "pitchAttackGain": 12, "pitchAttackSeconds": .024,
        "voiceDrive": 1.15, "busDrive": 1.08, "stereoWidth": 0,
    },
    "chords": {
        "source": "sine-fundamental", "secondSource": "variable-shape",
        "sourceBlend": .01, "secondSourceTranspose": 0,
        "secondSourceDetuneCents": 0, "shape": .46, "width": .5,
        "oscillator2Mix": .08, "detuneCents": -3.5,
        "attackSeconds": .002, "decaySeconds": .37, "sustain": .01,
        "releaseSeconds": .14, "velocitySensitivity": 0,
        "filterArchitecture": "state-variable-lowpass", "filterCutoffHz": 270,
        "highpassCutoffHz": 54, "filterEnvelopeHz": 4600,
        "filterEnvelopeDecaySeconds": .438, "filterEnvelopeSustain": 0,
        "filterKeyTracking": .28,
        "filterVelocitySensitivity": 0, "resonance": .46,
        "chorusMix": .34, "chorusDepth": .65, "chorusRateHz": .184,
        "stereoSpread": .28, "stereoWidth": 1.35,
    },
    "melody": {
        "source": "sine-fundamental", "secondSource": "additive-harmonic",
        "sourceBlend": .4, "secondSourceTranspose": 0,
        "secondSourceDetuneCents": 0, "harmonicFamily": 5, "fmIndex": .3,
        "attackSeconds": .001, "decaySeconds": .6, "sustain": .7,
        "releaseSeconds": .6, "velocitySensitivity": .5,
        "filterArchitecture": "source-direct", "voiceDrive": 1.08,
        "chorusMix": .5, "chorusDepth": .52, "chorusRateHz": 1.74,
        "reverbMix": .42, "reverbSeconds": 2.36, "reverbDamping": .64,
        "reverbPreDelaySeconds": .001, "stereoSpread": .045,
        "stereoWidth": 1.3,
    },
    "support": {
        "source": "variable-saw", "secondSource": "variable-shape",
        "sourceBlend": .28, "secondSourceTranspose": 0,
        "secondSourceDetuneCents": 0, "shape": .5, "width": .5,
        "oscillator2Mix": .22, "attackSeconds": .001,
        "decaySeconds": .12, "sustain": .01, "releaseSeconds": .06,
        "velocitySensitivity": .4, "filterArchitecture": "source-direct",
        "highpassCutoffHz": 100, "voiceDrive": 2, "busDrive": 1.12,
        "wavefold": 2.8,
        "chorusMix": .5, "chorusDepth": 1, "chorusRateHz": .24,
        "reverbMix": 0, "reverbSeconds": 1.2, "reverbDamping": .72,
        "reverbPreDelaySeconds": .003, "stereoSpread": .55,
        "stereoWidth": 1.5,
    },
}

CONTEXTS = {
    "bass": context_bass,
    "chords": context_chords,
    "melody": context_melody,
    "support": context_support,
}

DRUM_NOTES = {
    36: "kick", 38: "snare", 42: "closed-hat", 46: "open-hat",
    49: "crash", 50: "high-tom", 47: "mid-tom", 43: "floor-tom",
}


def note_events(sequence) -> list[dict]:
    active: dict[tuple[int, int], list[tuple[int, int]]] = {}
    notes: list[dict] = []
    for tick, _order, payload in sorted(sequence.events, key=lambda item: (item[0], item[1])):
        if len(payload) != 3:
            continue
        command = payload[0] & 0xF0
        key = (payload[0] & 0x0F, payload[1])
        if command == 0x90 and payload[2] > 0:
            active.setdefault(key, []).append((tick, payload[2]))
        elif command in (0x80, 0x90) and key in active and active[key]:
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


def drum_pattern() -> dict:
    events = []
    for item in note_events(context_drums()):
        piece = DRUM_NOTES.get(item["midi"])
        if piece:
            events.append({
                "piece": piece,
                "tick": round(item["startSeconds"] * 2 * DRUM_TICKS_PER_BEAT),
                "velocity": item["velocity"],
            })
    return {"bpm": 120, "beats": 64, "tailSeconds": 4, "events": events}


def deep_merge(base: dict, overlay: dict) -> dict:
    result = json.loads(json.dumps(base))
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = value
    return result


def render(executable: Path, request_file: Path, output_file: Path, request: dict) -> dict:
    request_file.write_text(json.dumps(request, separators=(",", ":")) + "\n", encoding="utf-8")
    environment = os.environ.copy()
    environment["PATH"] = str(REPO / "release") + os.pathsep + environment.get("PATH", "")
    completed = subprocess.run(
        [str(executable), "--render-instrument", str(request_file), str(output_file)],
        cwd=REPO, env=environment, text=True, capture_output=True, check=False,
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    result = json.loads(lines[-1])
    if not result.get("ok"):
        raise RuntimeError(str(result))
    return result


def melodic_request(role: str, patch: dict, events: list[dict], duration: float) -> dict:
    return {
        "schema": "jam2-instrument-patch-v1", "profileId": "pop_loop",
        "role": role, "audition": "reference-events", "patch": patch,
        "referenceEvents": events, "referenceDurationSeconds": duration,
    }


def read_pcm16(path: Path) -> tuple[int, list[tuple[float, float]]]:
    with wave.open(str(path), "rb") as source:
        if source.getsampwidth() != 2 or source.getnchannels() not in (1, 2):
            raise ValueError(f"Expected mono/stereo PCM16: {path}")
        rate = source.getframerate()
        channels = source.getnchannels()
        values = struct.unpack(f"<{source.getnframes() * channels}h", source.readframes(source.getnframes()))
    if channels == 1:
        return rate, [(value / 32768, value / 32768) for value in values]
    return rate, [(values[index] / 32768, values[index + 1] / 32768) for index in range(0, len(values), 2)]


def write_mix(path: Path, stems: dict[str, Path], gains: dict[str, float]) -> None:
    decoded = {role: read_pcm16(file) for role, file in stems.items()}
    rates = {value[0] for value in decoded.values()}
    if rates != {48000}:
        raise ValueError(f"Unexpected sample rates: {rates}")
    frames = max(len(value[1]) for value in decoded.values())
    mixed: list[tuple[float, float]] = []
    peak = 0.0
    for index in range(frames):
        left = right = 0.0
        for role, (_rate, samples) in decoded.items():
            if index < len(samples):
                left += samples[index][0] * gains[role]
                right += samples[index][1] * gains[role]
        peak = max(peak, abs(left), abs(right))
        mixed.append((left, right))
    scale = min(1.0, .98 / peak) if peak else 1.0
    packed = bytearray()
    for left, right in mixed:
        packed.extend(struct.pack("<hh", round(max(-1, min(1, left * scale)) * 32767), round(max(-1, min(1, right * scale)) * 32767)))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2); output.setsampwidth(2); output.setframerate(48000)
        output.writeframes(packed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build" / "jam2_sound_lab.exe")
    parser.add_argument("--kit", type=Path, default=ROOT / "tests" / "ableton-rock32-kit-request.json")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts" / "melodic-pop-compare")
    args = parser.parse_args()
    output = args.output.resolve(); requests = output / "requests"; stems = output / "context"
    requests.mkdir(parents=True, exist_ok=True); stems.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {"schema": "jam2-ableton-pop-render-v1", "renders": {}}
    stem_paths: dict[str, Path] = {}
    for role, sequence_factory in CONTEXTS.items():
        context_file = stems / f"{role}.wav"; stem_paths[role] = context_file
        context_result = render(args.executable.resolve(), requests / f"{role}-context.json", context_file,
            melodic_request(role, PATCHES[role], note_events(sequence_factory()), 36))
        diagnostic_events = [{"startSeconds": .2, "durationSeconds": .5, "midi": 60, "velocity": 80}]
        short_result = render(args.executable.resolve(), requests / f"{role}-short.json", output / f"{role}-short.wav",
            melodic_request(role, PATCHES[role], diagnostic_events, 3))
        long_events = [{"startSeconds": .2, "durationSeconds": 4, "midi": 60, "velocity": 80}]
        long_result = render(args.executable.resolve(), requests / f"{role}-long.json", output / f"{role}-long.wav",
            melodic_request(role, PATCHES[role], long_events, 8))
        report["renders"][role] = {"context": context_result, "short": short_result, "long": long_result}
        print(f"rendered {role}", flush=True)

    request_json = json.loads(args.kit.read_text(encoding="utf-8-sig"))
    kit = request_json.get("kit", request_json)
    pop_treatment = {"bus": {"drive": 1.08, "lowpassHz": 8000, "roomMix": .055, "stereoWidth": .13}, "pieces": {
        "kick": {"frequencyHz": 41, "decay": .045, "level": 1.5, "sourceLayerGain": .2},
        "snare": {"blend": .08, "level": .45},
        "closed-hat": {"level": .75}, "open-hat": {"level": .56},
        "crash": {"level": .52},
        "high-tom": {"level": .36}, "mid-tom": {"level": .39}, "floor-tom": {"level": .43}}}
    kit = deep_merge(kit, pop_treatment)
    drum_request = {"schema": "jam2-instrument-patch-v1", "profileId": "pop_loop", "role": "drums", "audition": "reference-pattern", "kit": kit, "referencePattern": drum_pattern()}
    drum_file = stems / "drums.wav"; stem_paths["drums"] = drum_file
    report["renders"]["drums"] = render(args.executable.resolve(), requests / "drums-context.json", drum_file, drum_request)
    write_mix(output / "full-mix.wav", stem_paths, {"bass": .73, "chords": .755, "melody": .428, "support": .403, "drums": .787})
    (output / "render-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
