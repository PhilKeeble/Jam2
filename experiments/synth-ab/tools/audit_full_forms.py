#!/usr/bin/env python3
"""Inspect full native-form Jam2 generations against research-facing data."""

from __future__ import annotations

import argparse
import array
import json
import math
import re
import statistics
import sys
import wave
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


CYMBALS = {"Closed HH", "Open HH", "Ride", "Crash"}
ACTIVE_BASS_PROFILES = {
    "pop_loop",
    "pop_sectional",
    "rock_riff_modal",
    "rock_shuffle_blues",
    "rock_punk_garage",
    "jazz_swing_standards",
    "jazz_bebop",
    "jazz_fusion",
    "blues_dominant",
    "blues_minor",
    "jpop_anisong_rock",
    "jpop_idol_dance",
    "country_honky_tonk",
    "country_contemporary",
    "electronic_techno",
    "electronic_breakbeat",
    "soul_classic_motown",
    "funk_static_pocket",
    "reggae_roots",
    "bossa_songbook",
}
SPARSE_BASS_PROFILES = {
    "modal_groove",
    "modal_atmospheric",
    "rnb_contemporary_neosoul",
    "hiphop_boom_bap",
    "hiphop_trap",
}
ARRANGEMENT_ROLES = {
    "lead_harmony",
    "countermelody",
    "call_response",
    "horn_stab",
    "hook_double",
    "drone",
    "pad",
}
TRAP_BASELINE_PROGRESSIONS = {
    "trap-1b6b7",
    "trap-1b3b74",
    "trap-descent",
    "trap-phrygian",
    "trap-sparse",
}
MODAL_PROGRESSION_MODES = {
    "modal-aeolian": "Natural Minor",
    "modal-dorian": "Dorian",
    "modal-lydian": "Lydian",
    "modal-mixolydian": "Mixolydian",
    "modal-phrygian": "Phrygian",
}


def load_pcm16(path: Path) -> tuple[int, array.array]:
    with wave.open(str(path), "rb") as source:
        if (
            source.getnchannels() != 1
            or source.getsampwidth() != 2
            or source.getcomptype() != "NONE"
        ):
            raise ValueError(f"{path}: expected mono PCM16")
        rate = source.getframerate()
        samples = array.array("h")
        samples.frombytes(source.readframes(source.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    return rate, samples


def wav_metrics(path: Path) -> dict[str, float | int]:
    rate, samples = load_pcm16(path)
    if not samples:
        return {"frames": 0, "seconds": 0.0, "peak": 0.0, "rms": 0.0, "dc": 0.0}
    peak = max(abs(value) for value in samples) / 32768.0
    rms = math.sqrt(sum(value * value for value in samples) / len(samples)) / 32768.0
    dc = sum(samples) / len(samples) / 32768.0
    ceiling = sum(abs(value) >= 32760 for value in samples)
    return {
        "frames": len(samples),
        "seconds": len(samples) / rate,
        "peak": peak,
        "rms": rms,
        "dc": dc,
        "ceiling_samples": ceiling,
    }


def integer_list(value: str | list[int]) -> list[int]:
    if isinstance(value, list):
        return [int(item) for item in value]
    return [int(item) for item in value.split() if item]


def latest_chord(chords: list[dict[str, Any]], tick: int) -> dict[str, Any] | None:
    active = None
    for chord in chords:
        if int(chord["tick"]) > tick:
            break
        active = chord
    return active


def roman_root(value: str) -> str:
    match = re.match(r"^[b#♭♯]?([ivIV]+)", value.strip())
    return match.group(1).upper() if match else ""


def section_harmony_signature(
    recipe: dict[str, Any], section: dict[str, Any]
) -> tuple[str, ...]:
    numerator = int(recipe["time"]["numerator"])
    first_beat = (int(section["start_bar"]) - 1) * numerator
    final_beat = first_beat + int(section["bars"]) * numerator
    return tuple(
        str(event["roman"])
        for event in recipe["progression"]["base_harmony"]
        if first_beat <= int(event["beat"]) < final_beat
    )


def sample_metrics(sample: dict[str, Any], site: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    recipe = sample["recipe"]
    time = recipe["time"]
    constraints = sample["research_constraints"]
    profile_id = sample["profile_id"]
    bars = int(recipe["bars"])
    numerator = int(time["numerator"])
    total_ticks = bars * numerator * 12
    lane_events = sample["lane_events"]
    drum_hits = sample["drum_hits"]
    chords = sorted(sample["chord_voicings"], key=lambda value: int(value["tick"]))
    lanes = Counter(event["role"] for event in lane_events)
    drum_lanes = Counter(hit["lane"] for hit in drum_hits)
    findings: list[dict[str, Any]] = []

    def finding(kind: str, detail: str, data: Any = None) -> None:
        value: dict[str, Any] = {"kind": kind, "detail": detail}
        if data is not None:
            value["data"] = data
        findings.append(value)

    if recipe["profile"]["id"] != profile_id:
        finding("identity", "Recipe profile differs from requested profile.")
    if recipe["native_form"]["id"] != sample["form_id"]:
        finding("identity", "Recipe form differs from requested form.")
    if bars != int(sample["requested_bars"]):
        finding("form", "Generated bar count differs from the native form.")
    if time["meter_id"] != sample["requested_meter"]:
        finding("meter", "Generated meter differs from the native form.")
    if time["meter_id"] not in constraints["meter_ids"]:
        finding("meter", "Generated meter is outside the profile catalogue.")
    bpm = int(recipe["bpm"])
    if not int(constraints["minimum_bpm"]) <= bpm <= int(constraints["maximum_bpm"]):
        finding("tempo", "Generated BPM is outside the researched profile range.")
    if recipe["progression"]["family_id"] not in constraints["progression_families"]:
        finding("harmony", "Progression family is outside the profile catalogue.")
    if any(bool(chord["close_root_seventh"]) for chord in chords):
        finding("voicing", "A close root/seventh pair survived the voicing policy.")
    if any(int(event["start_beat"] * 12) < 0 or int(event["start_beat"] * 12) >= total_ticks
           for event in lane_events):
        finding("bounds", "A pitched role event is outside the generated form.")
    if any(
        int(round((float(event["start_beat"]) + float(event["duration_beats"])) * 12))
        > total_ticks
        for event in lane_events
    ):
        finding("bounds", "A pitched role event extends beyond the generated form.")
    if any(int(hit["tick"]) < 0 or int(hit["tick"]) >= total_ticks for hit in drum_hits):
        finding("bounds", "A drum event is outside the generated form.")

    simultaneous: dict[int, list[str]] = defaultdict(list)
    for hit in drum_hits:
        simultaneous[int(hit["tick"])].append(hit["lane"])
    limb_excess = [
        (tick, values)
        for tick, values in simultaneous.items()
        if len(values) > 3 or sum(value in CYMBALS for value in values) > 1
    ]
    if limb_excess:
        finding("drums", "A drum position exceeds the compact limb/cymbal policy.", limb_excess[:4])

    melody_events = sorted(
        (event for event in lane_events if event["role"] == "melody"),
        key=lambda value: float(value["start_beat"]),
    )
    melody_section_counts: list[dict[str, Any]] = []
    for section in recipe["native_form"]["sections"]:
        first_beat = (int(section["start_bar"]) - 1) * numerator
        final_beat = first_beat + int(section["bars"]) * numerator
        melody_section_counts.append(
            {
                "label": section["label"],
                "start_bar": int(section["start_bar"]),
                "bars": int(section["bars"]),
                "events": sum(
                    first_beat <= float(event["start_beat"]) < final_beat
                    for event in melody_events
                ),
            }
        )
    empty_melody_sections = [
        section for section in melody_section_counts if section["events"] == 0
    ]
    if empty_melody_sections:
        finding(
            "melody-form",
            "One or more declared native-form sections has no melody event.",
            empty_melody_sections,
        )
    recipe_melody_events = recipe["motif"]["events"]
    if (
        recipe_melody_events
        and recipe_melody_events[-1]["chord_role"] ==
        "Intentional non-chord tone"
    ):
        finding(
            "melody-cadence",
            "The lead ends the form on an unresolved non-chord tone.",
            recipe_melody_events[-1],
        )
    melody_leaps = [
        abs(int(right["midi"]) - int(left["midi"]))
        for left, right in zip(melody_events, melody_events[1:])
    ]
    melody_collisions = 0
    for event in melody_events:
        chord = latest_chord(chords, round(float(event["start_beat"]) * 12))
        if not chord:
            continue
        voiced = integer_list(chord["midi"])
        if voiced and min(abs(int(event["midi"]) - note) for note in voiced) == 1:
            melody_collisions += 1

    support_events = [
        event
        for event in lane_events
        if event["role"] not in {"melody", "bass"}
    ]
    melody_by_tick: dict[int, list[int]] = defaultdict(list)
    for event in melody_events:
        melody_by_tick[round(float(event["start_beat"]) * 12)].append(
            int(event["midi"])
        )
    support_close_collisions = [
        {
            "tick": round(float(event["start_beat"]) * 12),
            "support_midi": int(event["midi"]),
            "melody_midi": melody_midi,
            "role": event["role"],
        }
        for event in support_events
        for melody_midi in melody_by_tick.get(
            round(float(event["start_beat"]) * 12), []
        )
        if abs(int(event["midi"]) - melody_midi) in {1, 2}
    ]
    if support_close_collisions:
        finding(
            "support-voicing",
            "A simultaneous support/lead onset forms an unlabelled semitone or whole-tone cluster.",
            support_close_collisions[:8],
        )

    foundation = [
        tool
        for tool in recipe["complexity_tools"]
        if int(tool["level"]) == 1 and not bool(tool["selected"])
    ]
    if foundation:
        finding(
            "complexity-teaching",
            "A realised level-one foundation is incorrectly reported as unselected.",
            [tool["tool_id"] for tool in foundation],
        )
    declared_roles = set(recipe["roles"]["supporting_roles"])
    if (
        int(recipe["complexity"]) == 8
        and declared_roles & ARRANGEMENT_ROLES
        and not support_events
    ):
        finding(
            "support-role",
            "A high-complexity generation declares arrangement-capable support roles but realises none.",
            sorted(declared_roles & ARRANGEMENT_ROLES),
        )

    bass_events = [event for event in lane_events if event["role"] == "bass"]
    bass_per_bar = len(bass_events) / max(bars, 1)
    if profile_id in ACTIVE_BASS_PROFILES and bass_per_bar + 1e-9 < numerator:
        finding(
            "bass-role",
            "Bass activity is below one onset per written beat for a profile whose research assigns it an active pulse or line.",
            {"events_per_bar": bass_per_bar, "written_beats_per_bar": numerator},
        )
    if profile_id in SPARSE_BASS_PROFILES and bass_per_bar > numerator:
        finding(
            "bass-role",
            "A deliberately sparse bass profile exceeds one onset per written beat.",
            {"events_per_bar": bass_per_bar, "written_beats_per_bar": numerator},
        )
    if profile_id == "metal_modern_progressive" and bass_per_bar < 1.0:
        finding(
            "bass-role",
            "Metal bass does not provide enough attacks to reinforce the riff.",
            {"events_per_bar": bass_per_bar},
        )
    if profile_id == "electronic_house":
        if bass_per_bar < 2.0:
            finding(
                "bass-role",
                "House bass does not establish its alternating offbeat cell.",
                {"events_per_bar": bass_per_bar},
            )
        chord_ticks = {int(chord["tick"]) for chord in chords}
        overlapping = [
            round(float(event["start_beat"]) * 12)
            for event in bass_events
            if round(float(event["start_beat"]) * 12) in chord_ticks
        ]
        if overlapping:
            finding(
                "house-interlock",
                "House bass and chord stabs mask one another on the same offbeat.",
                overlapping[:8],
            )

    if profile_id == "bossa_songbook":
        non_bossa = drum_lanes["Crash"] + drum_lanes["Tom"]
        if non_bossa:
            finding(
                "bossa-percussion",
                "Bossa contains generic crash or tom substitutions.",
                {"crash": drum_lanes["Crash"], "tom": drum_lanes["Tom"]},
            )
    if profile_id == "electronic_house":
        kick_beats = {
            round(float(hit["beat"]), 6)
            for hit in drum_hits
            if hit["lane"] == "Kick"
        }
        written_beats = bars * numerator
        anchored = sum(float(beat) in kick_beats for beat in range(written_beats))
        if anchored < written_beats:
            finding(
                "house-groove",
                "House is missing one or more four-on-the-floor written-beat kick anchors.",
                {"anchors": anchored, "expected": written_beats},
            )
        if chords and any(int(chord["tick"]) % 12 != 6 for chord in chords):
            finding(
                "house-comping",
                "House chord stabs are not consistently placed between quarter-note kicks.",
            )
    if profile_id == "hiphop_trap":
        if (
            int(recipe["complexity"]) < 5
            and recipe["progression"]["id"] not in TRAP_BASELINE_PROGRESSIONS
        ):
            finding(
                "trap-harmony",
                "Trap selected harmony outside its sparse Aeolian/Phrygian baseline vocabulary.",
                recipe["progression"]["id"],
            )
        snare_positions = [
            float(hit["beat"]) % 4.0
            for hit in drum_hits
            if hit["lane"] == "Snare" and hit["state"] != "g"
        ]
        if snare_positions and sum(abs(value - 2.0) < 0.01 for value in snare_positions) / len(snare_positions) < 0.75:
            finding(
                "trap-groove",
                "Trap backbeat is not predominantly on the half-time beat-three position.",
                snare_positions,
            )
    if profile_id == "hiphop_boom_bap":
        progression_id = recipe["progression"]["id"]
        if not progression_id.startswith("boombap-"):
            finding(
                "boombap-harmony",
                "Boom-Bap borrowed a Trap progression instead of an original sample-like loop.",
                progression_id,
            )
    if profile_id in {"modal_groove", "modal_atmospheric"}:
        progression_id = recipe["progression"]["id"]
        expected_mode = MODAL_PROGRESSION_MODES.get(progression_id)
        if expected_mode is None or recipe["mode"] != expected_mode:
            finding(
                "modal-collection",
                "The modal progression and declared collection do not agree.",
                {
                    "progression": progression_id,
                    "mode": recipe["mode"],
                    "expected": expected_mode,
                },
            )
    if profile_id == "funk_static_pocket" and recipe["mode"] not in {
        "Mixolydian",
        "Dorian",
    }:
        finding(
            "funk-collection",
            "Funk is outside its researched dominant/Mixolydian or Dorian centre.",
            recipe["mode"],
        )
    if profile_id == "rock_shuffle_blues" and recipe["mode"] != "Blues":
        finding(
            "blues-collection",
            "Shuffle / Blues Rock is not using its vocal-like Blues collection.",
            recipe["mode"],
        )
    if profile_id == "jazz_fusion" and "uptempo" in recipe["groove"]["id"]:
        finding(
            "fusion-groove",
            "Fusion selected the Swing profile's up-tempo ride instead of a straight electric pocket.",
            recipe["groove"]["id"],
        )
    if profile_id == "reggae_roots" and not (
        drum_lanes["Cross-stick / Rim"] or drum_lanes["Snare"]
    ):
        finding("reggae-groove", "Roots Reggae has no rim/snare riddim anchor.")
    if profile_id == "reggae_roots" and (
        not chords or any(int(chord["tick"]) % 12 != 6 for chord in chords)
    ):
        finding(
            "reggae-comping",
            "Roots Reggae upper chords do not preserve the offbeat skank.",
        )
    if profile_id == "bossa_songbook" and not any(
        int(chord["tick"]) % 12 != 0 for chord in chords
    ):
        finding(
            "bossa-comping",
            "Bossa upper voicings contain no syncopated onset.",
        )
    if profile_id == "funk_static_pocket" and not any(
        int(chord["tick"]) % 12 != 0 for chord in chords
    ):
        finding(
            "funk-comping",
            "Funk comping contains no interlocking offbeat onset.",
        )
    if profile_id == "rock_punk_garage":
        minimum_eighth_attacks = bars * numerator * 2
        if len(chords) < minimum_eighth_attacks:
            finding(
                "punk-comping",
                "Punk / Garage root attacks do not sustain the researched eighth-note drive.",
                {"onsets": len(chords), "expected": minimum_eighth_attacks},
            )
    if profile_id == "country_honky_tonk":
        incorrect = [
            int(chord["tick"])
            for chord in chords
            if (
                numerator == 3
                and (int(chord["tick"]) // 12) % numerator == 0
            )
            or (
                numerator != 3
                and (int(chord["tick"]) // 12) % numerator % 2 == 0
            )
        ]
        if incorrect:
            finding(
                "country-comping",
                "Honky-Tonk chord strums obscure the alternating bass slots.",
                incorrect[:8],
            )
    if profile_id == "metal_modern_progressive":
        kick_ticks = {
            int(hit["tick"])
            for hit in drum_hits
            if hit["lane"] == "Kick"
        }
        unaligned = [
            int(chord["tick"])
            for chord in chords
            if int(chord["tick"]) not in kick_ticks
        ]
        if unaligned:
            finding(
                "metal-kick-lock",
                "A heavy chord/riff attack is not aligned with its generated kick.",
                unaligned[:8],
            )
        unaligned_bass = [
            round(float(event["start_beat"]) * 12)
            for event in bass_events
            if round(float(event["start_beat"]) * 12)
            not in kick_ticks
        ]
        if unaligned_bass:
            finding(
                "metal-kick-lock",
                "A Metal bass attack is not aligned with its generated kick/riff.",
                unaligned_bass[:8],
            )
    if profile_id == "rock_punk_garage":
        expected_bass = bars * numerator * 2
        if len(bass_events) < expected_bass:
            finding(
                "punk-bass",
                "Punk / Garage bass does not sustain the researched eighth-note drive.",
                {"onsets": len(bass_events), "expected": expected_bass},
            )
    if profile_id in {"electronic_breakbeat", "funk_static_pocket"} and not any(
        round(float(event["start_beat"]) * 12) % 12 != 0
        for event in bass_events
    ):
        finding(
            "bass-syncopation",
            "The profile's interlocking bass grammar contains no offbeat onset.",
        )

    if recipe["progression"]["family_id"] == "blues_native_schema" and bars >= 12:
        harmonic_functions = {
            roman_root(event["roman"])
            for event in recipe["progression"]["base_harmony"]
        }
        missing_functions = sorted({"I", "IV", "V"} - harmonic_functions)
        if missing_functions:
            finding(
                "blues-form",
                "A native twelve- or sixteen-bar Blues form is missing a core I/IV/V function.",
                {
                    "functions": sorted(harmonic_functions),
                    "missing": missing_functions,
                },
            )
    if sample["form_id"] in {"jazz-blues-12", "bebop-blues-12"}:
        expected = (
            "jazz-blues-12"
            if sample["form_id"] == "jazz-blues-12"
            else "bebop-blues-12"
        )
        if (
            recipe["progression"]["family_id"] != "blues_native_schema"
            or recipe["progression"]["id"] != expected
        ):
            finding(
                "jazz-blues-form",
                "The Jazz Blues form is not using its native researched chorus.",
                {
                    "family": recipe["progression"]["family_id"],
                    "progression": recipe["progression"]["id"],
                },
            )
    if (
        profile_id == "jazz_swing_standards"
        and "blues" not in sample["form_id"]
        and "blues-12" in recipe["progression"]["id"]
    ):
        finding(
            "jazz-form",
            "A non-Blues Standards form selected a native twelve-bar Blues chorus.",
            recipe["progression"]["id"],
        )

    progression_id = recipe["progression"]["id"]
    complexity = int(recipe["complexity"])
    early_colour = {
        ("funk_static_pocket", "funk-chromatic"),
        ("bossa_songbook", "bossa-backdoor"),
        ("bossa_songbook", "bossa-cycle"),
        ("rnb_contemporary_neosoul", "rnb-backdoor"),
        ("rnb_contemporary_neosoul", "rnb-plagal"),
        ("pop_sectional", "pop-134m"),
        ("jpop_anisong_rock", "anime-134m"),
    }
    later_colour = {
        ("blues_dominant", "blues-jazz"),
        ("jazz_swing_standards", "jazz-backdoor"),
        ("hiphop_trap", "trap-14b65"),
    }
    profile_progression = (profile_id, progression_id)
    if complexity < 3 and profile_progression in early_colour:
        finding(
            "complexity-harmony",
            "A directed-colour progression appeared before its researched complexity stage.",
            progression_id,
        )
    if complexity < 5 and profile_progression in later_colour:
        finding(
            "complexity-harmony",
            "An expanded-vocabulary progression appeared before its researched complexity stage.",
            progression_id,
        )

    sections = recipe["native_form"]["sections"]
    if "abac" in sample["form_id"]:
        labels = [section["label"] for section in sections]
        if labels != ["A", "B", "A'", "C"]:
            finding(
                "section-form",
                "An ABAC form does not expose A-B-A'-C section semantics.",
                labels,
            )
    if sample["form_id"] in {
        "jazz-aaba-32",
        "jazz-abac-32",
        "bossa-aaba-32",
        "bossa-abac-32",
    }:
        signatures = {
            section["label"]: section_harmony_signature(recipe, section)
            for section in sections
        }
        if signatures.get("B") == signatures.get("A"):
            finding(
                "section-harmony",
                "The researched B/bridge section repeats the A harmony unchanged.",
                signatures,
            )
        if "abac" in sample["form_id"] and (
            signatures.get("C") == signatures.get("A")
            or signatures.get("C") == signatures.get("B")
        ):
            finding(
                "section-harmony",
                "The ABAC C ending does not provide a distinct harmonic route.",
                signatures,
            )
    section_signatures = {
        section["label"]: section_harmony_signature(recipe, section)
        for section in sections
    }
    if "Lift" in section_signatures and "B" in section_signatures:
        if section_signatures["Lift"] in {
            section_signatures.get("A"),
            section_signatures["B"],
        }:
            finding(
                "section-harmony",
                "A declared lift does not create its own route between A and B.",
                section_signatures,
            )
    audio: dict[str, Any] | None = None
    if sample["audio_mix"]:
        audio_path = site / sample["audio_mix"]
        if not audio_path.is_file():
            finding("audio", "The representative full-form mix is missing.")
        else:
            audio = wav_metrics(audio_path)
            expected_seconds = (
                bars * numerator * 60.0 /
                (bpm * int(time["tempo_pulse_units"]))
            )
            if abs(float(audio["seconds"]) - expected_seconds) > 0.03:
                finding(
                    "audio-duration",
                    "Rendered duration differs from meter/pulse/BPM calculation.",
                    {"actual": audio["seconds"], "expected": expected_seconds},
                )
            if int(audio["ceiling_samples"]) > 0 or abs(float(audio["dc"])) > 0.005:
                finding("audio-signal", "Rendered mix has ceiling or DC samples.", audio)

    root_count = len({int(chord["root_pitch_class"]) for chord in chords})
    metrics: dict[str, Any] = {
        "id": sample["id"],
        "style_id": sample["style_id"],
        "profile_id": profile_id,
        "form_id": sample["form_id"],
        "complexity": sample["requested_complexity"],
        "bars": bars,
        "meter": time["meter_id"],
        "bpm": bpm,
        "progression_family": recipe["progression"]["family_id"],
        "progression_id": recipe["progression"]["id"],
        "groove_id": recipe["groove"]["id"],
        "mode": recipe["mode"],
        "tonic": recipe["tonic"],
        "chord_onsets": len(chords),
        "unique_chord_roots": root_count,
        "lane_event_counts": dict(lanes),
        "drum_lane_counts": dict(drum_lanes),
        "bass_events_per_bar": bass_per_bar,
        "melody_events_per_bar": len(melody_events) / max(bars, 1),
        "melody_section_counts": melody_section_counts,
        "melody_range_semitones": (
            max(int(event["midi"]) for event in melody_events) -
            min(int(event["midi"]) for event in melody_events)
            if melody_events else 0
        ),
        "melody_max_leap": max(melody_leaps, default=0),
        "melody_minor_second_voicing_collisions": melody_collisions,
        "support_close_collisions": len(support_close_collisions),
        "support_events_per_bar": len(support_events) / max(bars, 1),
        "fingerprint": (
            recipe["fingerprints"]["chord"],
            recipe["fingerprints"]["beat"],
        ),
        "audio": audio,
        "finding_count": len(findings),
    }
    return metrics, findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "corpus",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "site" / "full-form-corpus.json",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    site = args.corpus.parent
    metrics: list[dict[str, Any]] = []
    findings: list[dict[str, Any]] = []
    for sample in corpus["samples"]:
        measured, sample_findings = sample_metrics(sample, site)
        metrics.append(measured)
        findings.extend(
            {"sample_id": sample["id"], **finding}
            for finding in sample_findings
        )

    by_cell: dict[tuple[str, str, int], list[dict[str, Any]]] = defaultdict(list)
    for value in metrics:
        by_cell[
            (value["profile_id"], value["form_id"], int(value["complexity"]))
        ].append(value)
    diversity_findings = []
    for cell, values in sorted(by_cell.items()):
        unique = len({value["fingerprint"] for value in values})
        if len(values) > 1 and unique < 2:
            diversity_findings.append(
                {
                    "sample_id": " / ".join(map(str, cell)),
                    "kind": "randomization",
                    "detail": (
                        "Repeated seeds produced no chord/drum fingerprint "
                        "variation in this profile/form/complexity cell."
                    ),
                    "data": {"samples": len(values), "unique_fingerprints": unique},
                }
            )
    findings.extend(diversity_findings)

    by_profile: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for value in metrics:
        by_profile[value["profile_id"]].append(value)
    summaries: list[dict[str, Any]] = []
    for profile_id, values in sorted(by_profile.items()):
        summary = {
            "profile_id": profile_id,
            "samples": len(values),
            "forms": sorted({value["form_id"] for value in values}),
            "meters": sorted({value["meter"] for value in values}),
            "modes": sorted({value["mode"] for value in values}),
            "progression_ids": sorted({value["progression_id"] for value in values}),
            "groove_ids": sorted({value["groove_id"] for value in values}),
            "tonics": sorted({value["tonic"] for value in values}),
            "bass_events_per_bar": {
                "minimum": min(value["bass_events_per_bar"] for value in values),
                "median": statistics.median(value["bass_events_per_bar"] for value in values),
                "maximum": max(value["bass_events_per_bar"] for value in values),
            },
            "melody_events_per_bar": {
                "minimum": min(value["melody_events_per_bar"] for value in values),
                "median": statistics.median(value["melody_events_per_bar"] for value in values),
                "maximum": max(value["melody_events_per_bar"] for value in values),
            },
            "maximum_melody_leap": max(value["melody_max_leap"] for value in values),
            "minor_second_voicing_collisions": sum(
                value["melody_minor_second_voicing_collisions"] for value in values
            ),
            "support_close_collisions": sum(
                value["support_close_collisions"] for value in values
            ),
            "support_events_per_bar": {
                "minimum": min(value["support_events_per_bar"] for value in values),
                "median": statistics.median(value["support_events_per_bar"] for value in values),
                "maximum": max(value["support_events_per_bar"] for value in values),
            },
            "findings": sum(value["finding_count"] for value in values),
        }
        summaries.append(summary)
        print(
            f"{profile_id:<28} samples={len(values):>3} "
            f"forms={len(summary['forms'])} "
            f"progressions={len(summary['progression_ids'])} "
            f"grooves={len(summary['groove_ids'])} "
            f"bass/bar={summary['bass_events_per_bar']['minimum']:.1f}-"
            f"{summary['bass_events_per_bar']['maximum']:.1f} "
            f"melody/bar={summary['melody_events_per_bar']['minimum']:.1f}-"
            f"{summary['melody_events_per_bar']['maximum']:.1f} "
            f"findings={summary['findings']}"
        )
    finding_kinds = Counter(finding["kind"] for finding in findings)
    report = {
        "corpus": str(args.corpus),
        "samples": len(metrics),
        "profiles": len(by_profile),
        "audio_mixes": sum(value["audio"] is not None for value in metrics),
        "finding_kinds": dict(sorted(finding_kinds.items())),
        "profile_summaries": summaries,
        "findings": findings,
        "sample_metrics": metrics,
    }
    output = args.output or args.corpus.with_name("full-form-audit-report.json")
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(
        f"SUMMARY samples={report['samples']} profiles={report['profiles']} "
        f"audio={report['audio_mixes']} findings={len(findings)} "
        f"kinds={dict(finding_kinds)}"
    )
    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
