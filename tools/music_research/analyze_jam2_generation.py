#!/usr/bin/env python3
"""Audit variation and full-arrangement structure in a Jam2 music corpus."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable, Sequence


SCHEMA = "jam2-symbolic-generation-audit-v1"
TICKS_PER_BEAT = 12

# GMD has broad top-level labels rather than Jam2's narrower production
# profiles. "proxy" rows are deliberately not treated as direct validation.
GMD_PROFILE_MAP: dict[str, tuple[str | None, str, str]] = {
    "pop_loop": ("pop", "direct", "GMD Pop"),
    "pop_sectional": ("pop", "direct", "GMD Pop"),
    "rock_riff_modal": ("rock", "direct", "GMD Rock"),
    "rock_shuffle_blues": ("blues", "proxy", "GMD Blues proxy; Rock articulation is narrower"),
    "rock_punk_garage": ("punk", "direct", "GMD Punk"),
    "jazz_swing_standards": ("jazz", "direct", "GMD Jazz"),
    "jazz_bebop": ("jazz", "proxy", "GMD Jazz proxy; sparse Bebop-specific coverage"),
    "jazz_fusion": ("jazz", "proxy", "GMD Jazz proxy; straight/odd Fusion is underrepresented"),
    "modal_groove": ("jazz", "proxy", "GMD Jazz proxy for human-kit behaviour only"),
    "modal_atmospheric": (None, "unsupported", "No suitable GMD style population"),
    "blues_dominant": ("blues", "direct", "GMD Blues"),
    "blues_minor": ("blues", "direct", "GMD Blues"),
    "jpop_anisong_rock": ("rock", "proxy", "GMD Rock proxy; no Anisong label"),
    "jpop_idol_dance": ("dance", "proxy", "GMD Dance proxy; no J-Pop label"),
    "country_honky_tonk": ("country", "direct", "GMD Country"),
    "country_contemporary": ("country", "direct", "GMD Country"),
    "electronic_house": ("dance/disco", "proxy", "GMD Disco proxy; House production and form remain distinct"),
    "electronic_techno": ("dance", "proxy", "GMD Dance proxy; not Techno-specific"),
    "electronic_breakbeat": ("dance/breakbeat", "direct", "GMD Breakbeat label; only two source performances"),
    "soul_classic_motown": ("soul", "direct", "GMD Soul"),
    "rnb_contemporary_neosoul": ("soul", "proxy", "GMD Soul proxy; modern Neo-Soul is underrepresented"),
    "funk_static_pocket": ("funk", "direct", "GMD Funk"),
    "hiphop_boom_bap": ("hiphop", "direct", "GMD Hip-hop"),
    "hiphop_trap": ("hiphop", "proxy", "GMD Hip-hop proxy; Trap is underrepresented"),
    "reggae_roots": ("reggae", "direct", "GMD Reggae"),
    "bossa_songbook": (
        "latin/brazilian-bossa",
        "direct",
        "GMD Brazilian Bossa label; only two unusually long source "
        "performances, so its density and timing remain descriptive",
    ),
    "metal_modern_progressive": ("rock", "proxy", "GMD Rock proxy; Metalcore requires complementary evidence"),
}

GMD_EXACT_STYLE_PROFILES = {
    "electronic_house",
    "electronic_breakbeat",
    "bossa_songbook",
}

GMD_LANE_NAMES = {
    "kick": "kick",
    "snare": "snare",
    "closed_hat": "closed-hat",
    "open_hat": "open-hat",
    "ride": "ride",
    "crash": "crash",
    "high_tom": "high-tom",
    "mid_tom": "mid-tom",
    "floor_tom": "floor-tom",
    "cross_stick": "cross-stick",
}

NOTE_PITCH_CLASSES = {
    "C": 0,
    "C#": 1,
    "DB": 1,
    "D": 2,
    "D#": 3,
    "EB": 3,
    "E": 4,
    "F": 5,
    "F#": 6,
    "GB": 6,
    "G": 7,
    "G#": 8,
    "AB": 8,
    "A": 9,
    "A#": 10,
    "BB": 10,
    "B": 11,
}
NATURAL_PITCH_CLASSES = {
    "C": 0,
    "D": 2,
    "E": 4,
    "F": 5,
    "G": 7,
    "A": 9,
    "B": 11,
}
MODE_INTERVALS = {
    "IONIAN": {0, 2, 4, 5, 7, 9, 11},
    "MAJOR": {0, 2, 4, 5, 7, 9, 11},
    "NATURAL MINOR": {0, 2, 3, 5, 7, 8, 10},
    "AEOLIAN": {0, 2, 3, 5, 7, 8, 10},
    "DORIAN": {0, 2, 3, 5, 7, 9, 10},
    "MIXOLYDIAN": {0, 2, 4, 5, 7, 9, 10},
    "LYDIAN": {0, 2, 4, 6, 7, 9, 11},
    "PHRYGIAN": {0, 1, 3, 5, 7, 8, 10},
    "MAJOR PENTATONIC": {0, 2, 4, 7, 9},
}
MODE_CHARACTERISTIC_INTERVALS = {
    "DORIAN": 9,
    "MIXOLYDIAN": 10,
    "NATURAL MINOR": 8,
    "AEOLIAN": 8,
    "PHRYGIAN": 1,
    "LYDIAN": 6,
}

CHORD_RE = re.compile(
    r"^([A-Ga-g])([#b]?)([^/]*)(?:/([A-Ga-g])([#b]?))?$"
)


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return float(ordered[lower] * (1.0 - weight) + ordered[upper] * weight)


def summarize(values: Iterable[float], digits: int = 4) -> dict[str, float | int]:
    materialized = [float(value) for value in values]
    if not materialized:
        return {"count": 0}
    return {
        "count": len(materialized),
        "minimum": round(min(materialized), digits),
        "p05": round(percentile(materialized, 0.05), digits),
        "median": round(statistics.median(materialized), digits),
        "mean": round(statistics.fmean(materialized), digits),
        "p95": round(percentile(materialized, 0.95), digits),
        "maximum": round(max(materialized), digits),
    }


def ratio(numerator: int | float, denominator: int | float) -> float:
    return round(float(numerator) / float(denominator), 4) if denominator else 0.0


def stable_digest(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def tonic_pitch_class(name: str) -> int | None:
    normalized = (
        name.strip()
        .replace("♯", "#")
        .replace("♭", "b")
        .upper()
    )
    return note_pitch_class(normalized)


def note_pitch_class(name: str) -> int | None:
    normalized = (
        name.strip()
        .replace("♯", "#")
        .replace("♭", "b")
        .upper()
    )
    if not normalized or normalized[0] not in NATURAL_PITCH_CLASSES:
        return None
    pitch = NATURAL_PITCH_CLASSES[normalized[0]]
    for accidental in normalized[1:]:
        if accidental == "#":
            pitch += 1
        elif accidental == "B":
            pitch -= 1
        else:
            return None
    return pitch % 12


def chord_details(symbol: str) -> dict[str, Any] | None:
    match = CHORD_RE.match(symbol.strip())
    if not match:
        return None
    root_name = (match.group(1) + match.group(2)).upper()
    root = note_pitch_class(root_name)
    if root is None:
        return None
    suffix = match.group(3).lower()
    is_diminished = (
        "dim" in suffix or
        "Â°" in suffix or
        "°" in suffix
    )
    if "m7b5" in suffix or "Ã¸" in suffix or "ø" in suffix:
        intervals = {0, 3, 6, 10}
    elif is_diminished and "7" in suffix:
        intervals = {0, 3, 6, 9}
    elif is_diminished:
        intervals = {0, 3, 6}
    elif suffix.startswith("m") and not suffix.startswith("maj"):
        intervals = {0, 3, 7}
    else:
        intervals = {0, 4, 7}
    if suffix in {"6", "m6"}:
        intervals.add(9)
    if (
        "maj7" in suffix or suffix.startswith("maj9")
    ) and not is_diminished:
        intervals.add(11)
    elif (
        "7" in suffix or
        suffix.startswith("m9") or
        suffix in {"9", "13"}
    ) and not is_diminished and "m7b5" not in suffix:
        intervals.add(10)
    if "b9" in suffix:
        intervals.add(1)
    elif "#9" in suffix:
        intervals.add(3)
    elif "9" in suffix or "add9" in suffix:
        intervals.add(2)
    if "#11" in suffix:
        intervals.add(6)
    if suffix == "13":
        intervals.update({2, 9})
    bass = root
    if match.group(4):
        bass_name = (match.group(4) + match.group(5)).upper()
        bass = note_pitch_class(bass_name)
        if bass is None:
            bass = root
    pitch_classes = {(root + interval) % 12 for interval in intervals}
    pitch_classes.add(bass)
    return {
        "root": root,
        "bass": bass,
        "pitch_classes": pitch_classes,
        "suffix": suffix,
    }


def active_mode_pitch_classes(recipe: dict[str, Any]) -> set[int]:
    tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
    intervals = MODE_INTERVALS.get(
        str(recipe.get("mode", "")).strip().upper()
    )
    if tonic is None or intervals is None:
        return set()
    return {(tonic + interval) % 12 for interval in intervals}


def final_harmony_events(recipe: dict[str, Any]) -> list[dict[str, Any]]:
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    rows = []
    for text in recipe.get("progression", {}).get("final_chord_plan", []):
        match = re.match(r"^(\d+):(\d+)\s+(.+)$", str(text))
        if not match:
            continue
        bar = int(match.group(1))
        within = int(match.group(2))
        rows.append({
            "beat": (bar - 1) * beats_per_bar + within - 1,
            "chord": match.group(3).strip(),
        })
    return sorted(rows, key=lambda row: int(row["beat"]))


def first_event_at_tick(
    events: Sequence[dict[str, Any]],
    tick: int,
) -> dict[str, Any] | None:
    return next(
        (event for event in events if int(event.get("tick", -1)) == tick),
        None,
    )


def first_event_in_tick_span(
    events: Sequence[dict[str, Any]],
    start_tick: int,
    end_tick: int,
) -> dict[str, Any] | None:
    """Return the first onset in a harmonic span, including its boundary."""
    exact = first_event_at_tick(events, start_tick)
    if exact:
        return exact
    return next(
        (
            event for event in events
            if start_tick < int(event.get("tick", -1)) < end_tick
        ),
        None,
    )


def sounding_event_at_tick(
    events: Sequence[dict[str, Any]],
    tick: int,
) -> dict[str, Any] | None:
    return next(
        (
            event for event in events
            if int(event.get("tick", -1)) <= tick
            < (
                int(event.get("tick", -1))
                + int(event.get("duration_ticks", 0))
            )
        ),
        None,
    )


def theory_operation_audit(recipe: dict[str, Any]) -> list[dict[str, Any]]:
    """Validate that applied theory devices function and coordinate by role."""
    decisions = recipe.get("progression", {}).get("theory_decisions", [])
    harmony = final_harmony_events(recipe)
    melody = role_events(recipe, "melody")
    bass = role_events(recipe, "bass")
    support = role_events(recipe, "support")
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    phrase_beats = (
        max(1, int(recipe.get("native_form", {}).get("phrase_bars", 4)))
        * beats_per_bar
    )
    tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
    mode_pitch_classes = active_mode_pitch_classes(recipe)
    rows = []
    for decision in decisions:
        kind = str(decision.get("kind", ""))
        beat = int(decision.get("beat", -1))
        tick = beat * TICKS_PER_BEAT
        after_text = str(decision.get("after", ""))
        before_text = str(decision.get("before", ""))
        target_text = str(decision.get("resolution_target", ""))
        before = chord_details(before_text)
        after = chord_details(after_text)
        target = chord_details(target_text)
        at_index = next(
            (
                index for index, event in enumerate(harmony)
                if int(event["beat"]) == beat
            ),
            -1,
        )
        actual = harmony[at_index] if at_index >= 0 else None
        next_harmony = (
            harmony[at_index + 1]
            if at_index >= 0 and at_index + 1 < len(harmony)
            else None
        )
        next_details = (
            chord_details(str(next_harmony["chord"]))
            if next_harmony else None
        )
        resolution_harmony = next_harmony
        failures: list[str] = []
        if not actual or str(actual["chord"]) != after_text:
            failures.append("decision chord is absent from the final harmony")
        functional_target = target
        if kind in {"diatonic-extension", "country-extension"}:
            functional_target = None
            if (
                kind == "diatonic-extension"
                and after
                and mode_pitch_classes
                and not (
                    after["pitch_classes"] <= mode_pitch_classes
                )
            ):
                failures.append(
                    "mode-derived extension leaves the active collection"
                )
        elif kind == "modal-interchange":
            # A borrowed subdominant may move through a short applied
            # dominant before reaching the tonic it is preparing. Treat that
            # as one functional gesture, while keeping the search bounded so
            # a later unrelated tonic cannot disguise a failed resolution.
            resolution_harmony = next(
                (
                    event for event in harmony[
                        at_index + 1:at_index + 3
                    ]
                    if target
                    and chord_details(str(event["chord"]))
                    and chord_details(str(event["chord"]))["root"]
                    == target["root"]
                ),
                None,
            ) if at_index >= 0 else None
            if not target or not resolution_harmony:
                failures.append(
                    "borrowed subdominant does not reach its claimed tonic "
                    "within the following functional gesture"
                )
        elif kind in {
            "secondary-dominant",
            "passing-diminished",
            "backdoor-dominant",
            "tritone-substitution",
        }:
            if not next_details or not target:
                failures.append("claimed resolution has no following harmonic target")
            elif next_details["root"] != target["root"]:
                failures.append("following harmony does not match the claimed resolution")
        if after and target:
            relation = (after["root"] - target["root"]) % 12
            if kind == "secondary-dominant" and relation != 7:
                failures.append("applied dominant is not V of its destination")
            elif kind == "passing-diminished" and (
                relation != 11 or "dim" not in after["suffix"]
            ):
                failures.append("passing diminished is not a semitone below its destination")
            elif kind == "backdoor-dominant" and relation != 10:
                failures.append("backdoor dominant is not bVII of tonic")
            elif kind == "tritone-substitution" and relation != 1:
                failures.append("tritone substitute is not bII of tonic")
        if kind == "inversion" and after and after["bass"] == after["root"]:
            failures.append("inversion has no written non-root bass")
        if (
            kind == "inversion"
            and before
            and after
            and at_index > 0
        ):
            previous_details = chord_details(
                str(harmony[at_index - 1]["chord"])
            )

            def pitch_class_distance(left: int, right: int) -> int:
                distance = abs(left - right) % 12
                return min(distance, 12 - distance)

            if previous_details:
                root_cost = pitch_class_distance(
                    previous_details["bass"], before["root"]
                )
                inversion_cost = pitch_class_distance(
                    previous_details["bass"], after["bass"]
                )
                if next_details:
                    root_cost += pitch_class_distance(
                        before["root"], next_details["bass"]
                    )
                    inversion_cost += pitch_class_distance(
                        after["bass"], next_details["bass"]
                    )
                if inversion_cost >= root_cost:
                    failures.append(
                        "inversion does not improve the surrounding bass "
                        "route"
                    )

        melody_at = sounding_event_at_tick(melody, tick)
        decision_end_tick = (
            int(next_harmony["beat"]) * TICKS_PER_BEAT
            if next_harmony
            else int(recipe.get("bars", 1))
                * beats_per_bar * TICKS_PER_BEAT
        )
        bass_at = first_event_in_tick_span(
            bass, tick, decision_end_tick
        )
        support_at = sounding_event_at_tick(support, tick)
        if after:
            if kind != "inversion" and melody_at:
                melody_pitch = int(
                    melody_at.get("midi", -1)
                ) % 12
                allowed_melody = (
                    mode_pitch_classes
                    if kind == "diatonic-extension" and
                    mode_pitch_classes
                    else after["pitch_classes"]
                )
                if melody_pitch not in allowed_melody:
                    failures.append(
                        "sounding melody conflicts with the altered harmony"
                    )
            if (
                not bass_at
                or int(bass_at.get("midi", -1)) % 12 != after["bass"]
            ):
                failures.append("bass does not state the altered chord root or slash bass")
            if support_at:
                support_pitch = int(
                    support_at.get("midi", -1)
                ) % 12
                allowed_support = (
                    mode_pitch_classes
                    if kind == "diatonic-extension" and
                    mode_pitch_classes
                    else after["pitch_classes"]
                )
                if support_pitch not in allowed_support:
                    failures.append(
                        "support note conflicts with the altered harmony"
                    )

        resolution_tick = (
            int(resolution_harmony["beat"]) * TICKS_PER_BEAT
            if resolution_harmony else -1
        )
        melody_resolution = (
            sounding_event_at_tick(melody, resolution_tick)
            if resolution_tick >= 0 else None
        )
        resolution_index = (
            harmony.index(resolution_harmony)
            if resolution_harmony in harmony
            else -1
        )
        resolution_end_tick = (
            int(harmony[resolution_index + 1]["beat"])
            * TICKS_PER_BEAT
            if resolution_index >= 0
            and resolution_index + 1 < len(harmony)
            else int(recipe.get("bars", 1))
                * beats_per_bar * TICKS_PER_BEAT
        )
        bass_resolution = (
            first_event_in_tick_span(
                bass,
                resolution_tick,
                resolution_end_tick,
            )
            if resolution_tick >= 0 else None
        )
        realized_resolution = (
            chord_details(str(resolution_harmony["chord"]))
            if resolution_harmony else None
        )
        if functional_target and kind != "inversion" and resolution_harmony:
            if (
                melody_resolution
                and int(melody_resolution.get("midi", -1)) % 12
                not in (
                    realized_resolution or functional_target
                )["pitch_classes"]
            ):
                failures.append("sounding melody conflicts with the resolution harmony")
            if (
                not bass_resolution
                or int(bass_resolution.get("midi", -1)) % 12
                != (realized_resolution or functional_target)["bass"]
            ):
                failures.append("bass does not state the resolution root or slash bass")

        target_beat = (
            int(next_harmony["beat"])
            if kind in {
                "secondary-dominant",
                "passing-diminished",
                "backdoor-dominant",
                "tritone-substitution",
            } and next_harmony
            else int(resolution_harmony["beat"])
            if kind == "modal-interchange"
            and resolution_harmony
            else beat
        )
        section = next(
            (
                section for section
                in recipe.get("native_form", {}).get("sections", [])
                if (
                    (int(section.get("start_bar", 1)) - 1)
                    * beats_per_bar
                    <= target_beat
                    < (
                        int(section.get("start_bar", 1))
                        - 1
                        + int(section.get("bars", 1))
                    )
                    * beats_per_bar
                )
            ),
            {},
        )
        section_start = (
            (int(section.get("start_bar", 1)) - 1) * beats_per_bar
            == target_beat
        ) if section else False
        rows.append({
            "kind": kind,
            "beat": beat,
            "bar": beat // beats_per_bar + 1,
            "after": after_text,
            "resolution_target": target_text,
            "actual_next_chord": (
                str(next_harmony["chord"]) if next_harmony else ""
            ),
            "actual_resolution_chord": (
                str(resolution_harmony["chord"])
                if resolution_harmony else ""
            ),
            "actual_resolution_beat": (
                int(resolution_harmony["beat"])
                if resolution_harmony else -1
            ),
            "section": str(section.get("label", "")),
            "target_at_phrase_start": target_beat % phrase_beats == 0,
            "target_at_section_start": section_start,
            "melody_at_decision": bool(melody_at),
            "melody_midi_at_decision": (
                int(melody_at.get("midi", -1)) if melody_at else -1
            ),
            "bass_at_decision": bool(bass_at),
            "bass_midi_at_decision": (
                int(bass_at.get("midi", -1)) if bass_at else -1
            ),
            "support_at_decision": bool(support_at),
            "support_midi_at_decision": (
                int(support_at.get("midi", -1)) if support_at else -1
            ),
            "support_role_at_decision": (
                str(support_at.get("role", "")) if support_at else ""
            ),
            "support_relationship_at_decision": (
                str(support_at.get("relationship", ""))
                if support_at else ""
            ),
            "melody_midi_at_resolution": (
                int(melody_resolution.get("midi", -1))
                if melody_resolution else -1
            ),
            "melody_role_at_resolution": (
                str(melody_resolution.get("melodic_role", ""))
                if melody_resolution else ""
            ),
            "bass_midi_at_resolution": (
                int(bass_resolution.get("midi", -1))
                if bass_resolution else -1
            ),
            "failures": failures,
        })
    return rows


def role_events(recipe: dict[str, Any], lane: str) -> list[dict[str, Any]]:
    if lane == "melody":
        return list(recipe.get("motif", {}).get("events", []))
    key = "bass_events" if lane == "bass" else "supporting_events"
    return list(recipe.get("roles", {}).get(key, []))


def event_signature(
    events: Sequence[dict[str, Any]],
    normalized: bool,
    melody: bool = False,
) -> tuple[tuple[Any, ...], ...]:
    if not events:
        return ()
    first_midi = int(events[0].get("midi", 0))
    result: list[tuple[Any, ...]] = []
    for event in events:
        midi = int(event.get("midi", 0))
        pitch = midi - first_midi if normalized else midi
        descriptive_role = (
            str(event.get("melodic_role", "")),
            str(event.get("chord_role", "")),
        ) if melody else (
            str(event.get("role", "")),
            str(event.get("relationship", "")),
        )
        result.append(
            (
                int(event.get("tick", 0)),
                int(event.get("duration_ticks", 0)),
                pitch,
                *descriptive_role,
                str(event.get("articulation", "")),
            )
        )
    return tuple(result)


def harmony_signature(
    recipe: dict[str, Any], normalized: bool
) -> tuple[tuple[Any, ...], ...]:
    events = recipe.get("progression", {}).get("base_harmony", [])
    return tuple(
        (
            int(event.get("beat", 0)),
            int(event.get("duration_beats", 0)),
            str(event.get("roman", "")),
            "" if normalized else str(event.get("chord", "")),
        )
        for event in events
    )


def drum_signature(
    recipe: dict[str, Any], expressive: bool
) -> tuple[tuple[Any, ...], ...]:
    events = recipe.get("groove", {}).get("performance_events", [])
    return tuple(
        (
            int(event.get("tick", 0)),
            str(event.get("lane", "")),
            str(event.get("role", "")),
            bool(event.get("fill", False)),
            *(
                (
                    int(event.get("velocity", 0)),
                    int(event.get("offset_ms", 0)),
                    str(event.get("articulation", "")),
                )
                if expressive
                else ()
            ),
        )
        for event in events
    )


def bar_index_for_tick(tick: int, beats_per_bar: int) -> int:
    return tick // max(1, beats_per_bar * TICKS_PER_BEAT)


def drum_backbone_bar_signatures(
    recipe: dict[str, Any],
) -> tuple[tuple[tuple[int, str], ...], ...]:
    bars = max(1, int(recipe.get("bars", 1)))
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    bar_ticks = beats_per_bar * TICKS_PER_BEAT
    backbone_lanes = {
        "kick",
        "snare",
        "cross_stick",
        "closed_hat",
        "open_hat",
        "ride",
    }
    backbone: list[list[tuple[int, str]]] = [[] for _ in range(bars)]
    for event in recipe.get("groove", {}).get("performance_events", []):
        tick = int(event.get("tick", 0))
        bar = bar_index_for_tick(tick, beats_per_bar)
        if bar < 0 or bar >= bars:
            continue
        lane = str(event.get("lane", ""))
        articulation = str(event.get("articulation", ""))
        if (
            lane in backbone_lanes
            and not bool(event.get("fill", False))
            and str(event.get("role", "")) != "ghost"
            and "ghost" not in articulation
        ):
            backbone[bar].append((tick % bar_ticks, lane))
    return tuple(tuple(sorted(row)) for row in backbone)


def drum_flow_metrics(recipe: dict[str, Any]) -> dict[str, Any]:
    """Describe backbone reuse separately from bounded performance detail."""
    bars = max(1, int(recipe.get("bars", 1)))
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    bar_ticks = beats_per_bar * TICKS_PER_BEAT
    events = recipe.get("groove", {}).get("performance_events", [])
    detailed: list[list[tuple[int, str, str, bool]]] = [
        [] for _ in range(bars)
    ]
    fill_bars: set[int] = set()
    for event in events:
        tick = int(event.get("tick", 0))
        bar = bar_index_for_tick(tick, beats_per_bar)
        if bar < 0 or bar >= bars:
            continue
        lane = str(event.get("lane", ""))
        role = str(event.get("role", ""))
        fill = bool(event.get("fill", False))
        within = tick % bar_ticks
        detailed[bar].append((within, lane, role, fill))
        if fill:
            fill_bars.add(bar)
    # Ghost notes, fills, crashes, and tom orchestration are the bounded
    # variations around the timekeeping/kick/snare backbone.
    backbone_signatures = list(drum_backbone_bar_signatures(recipe))
    detailed_signatures = [tuple(sorted(row)) for row in detailed]
    adjacent_changes = sum(
        left != right
        for left, right in zip(backbone_signatures, backbone_signatures[1:])
    )
    phrase_bars = max(1, int(
        recipe.get("native_form", {}).get("phrase_bars", 4)
    ))
    phrase_signatures = [
        tuple(backbone_signatures[start:start + phrase_bars])
        for start in range(0, bars, phrase_bars)
    ]
    section_end_bars = {
        int(section.get("start_bar", 1))
        + int(section.get("bars", 1))
        - 2
        for section in recipe.get("native_form", {}).get("sections", [])
    }
    phrase_end_bars = set(range(phrase_bars - 1, bars, phrase_bars))
    structural_fill_bars = fill_bars & (section_end_bars | phrase_end_bars)
    return {
        "unique_backbone_bars": len(set(backbone_signatures)),
        "unique_detailed_bars": len(set(detailed_signatures)),
        "backbone_reuse_ratio": ratio(
            bars - len(set(backbone_signatures)), bars
        ),
        "adjacent_backbone_change_ratio": ratio(
            adjacent_changes, max(0, bars - 1)
        ),
        "phrase_count": len(phrase_signatures),
        "unique_backbone_phrases": len(set(phrase_signatures)),
        "fill_bar_count": len(fill_bars),
        "fill_bar_ratio": ratio(len(fill_bars), bars),
        "structural_fill_ratio": ratio(
            len(structural_fill_bars), len(fill_bars)
        ),
        "fill_bars": sorted(bar + 1 for bar in fill_bars),
    }


def section_melody_recurrence(recipe: dict[str, Any]) -> dict[str, Any]:
    """Measure whether later sections audibly recall earlier melodic material."""
    events = role_events(recipe, "melody")
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    rows: list[tuple[tuple[int, int, int], ...]] = []
    labels: list[str] = []
    for section in recipe.get("native_form", {}).get("sections", []):
        start_bar = max(1, int(section.get("start_bar", 1)))
        bars = max(1, int(section.get("bars", 1)))
        start_tick = (start_bar - 1) * beats_per_bar * TICKS_PER_BEAT
        end_tick = (start_bar - 1 + bars) * beats_per_bar * TICKS_PER_BEAT
        selected = [
            event for event in events
            if start_tick <= int(event.get("tick", 0)) < end_tick
        ]
        first_midi = int(selected[0].get("midi", 0)) if selected else 0
        rows.append(tuple(
            (
                int(event.get("tick", 0)) - start_tick,
                int(event.get("duration_ticks", 0)),
                int(event.get("midi", 0)) - first_midi,
            )
            for event in selected
        ))
        labels.append(str(section.get("label", "")))
    pair_rows = []
    for left, right in pair_indices(len(rows)):
        pair_rows.append({
            "left": labels[left],
            "right": labels[right],
            "similarity": round(
                jaccard(signature_set(rows[left]), signature_set(rows[right])),
                4,
            ),
        })
    return {
        "section_pairs": pair_rows,
        "pair_similarity": summarize(
            float(row["similarity"]) for row in pair_rows
        ),
        "first_to_final_similarity": (
            round(jaccard(signature_set(rows[0]), signature_set(rows[-1])), 4)
            if len(rows) > 1 else 1.0
        ),
    }


def four_bar_flow_metrics(recipe: dict[str, Any]) -> dict[str, Any]:
    """Describe whole-form development without assuming repetition is bad."""
    bars = max(1, int(recipe.get("bars", 1)))
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    block_bars = min(4, bars)
    block_beats = block_bars * beats_per_bar
    block_ticks = block_beats * TICKS_PER_BEAT
    block_count = math.ceil(bars / block_bars)
    harmony = final_harmony_events(recipe)
    lane_events = {
        "melody": role_events(recipe, "melody"),
        "bass": role_events(recipe, "bass"),
        "support": role_events(recipe, "support"),
        "drums": recipe.get("groove", {}).get("performance_events", []),
    }
    signatures: dict[str, list[tuple[tuple[Any, ...], ...]]] = {
        "harmony": [],
        **{lane: [] for lane in lane_events},
    }
    for block in range(block_count):
        start_beat = block * block_beats
        end_beat = min(bars * beats_per_bar, start_beat + block_beats)
        signatures["harmony"].append(tuple(
            (
                int(event["beat"]) - start_beat,
                str(event["chord"]),
            )
            for event in harmony
            if start_beat <= int(event["beat"]) < end_beat
        ))
        start_tick = block * block_ticks
        end_tick = min(
            bars * beats_per_bar * TICKS_PER_BEAT,
            start_tick + block_ticks,
        )
        for lane, events in lane_events.items():
            selected = [
                event for event in events
                if start_tick <= int(event.get("tick", 0)) < end_tick
            ]
            first_midi = (
                int(selected[0].get("midi", 0))
                if selected and lane != "drums" else 0
            )
            rows = []
            for event in selected:
                if lane == "drums":
                    rows.append((
                        int(event.get("tick", 0)) - start_tick,
                        str(event.get("lane", "")),
                        bool(event.get("fill", False)),
                    ))
                else:
                    rows.append((
                        int(event.get("tick", 0)) - start_tick,
                        int(event.get("duration_ticks", 0)),
                        int(event.get("midi", 0)) - first_midi,
                    ))
            signatures[lane].append(tuple(rows))
    result: dict[str, Any] = {
        "block_bars": block_bars,
        "block_count": block_count,
        "lanes": {},
    }
    for lane, rows in signatures.items():
        adjacent = [
            jaccard(signature_set(left), signature_set(right))
            for left, right in zip(rows, rows[1:])
        ]
        exact_adjacent = sum(
            left == right for left, right in zip(rows, rows[1:])
        )
        result["lanes"][lane] = {
            "unique_block_ratio": ratio(len(set(rows)), len(rows)),
            "exact_adjacent_repeat_ratio": ratio(
                exact_adjacent, max(0, len(rows) - 1)
            ),
            "adjacent_similarity": summarize(adjacent),
            "first_to_final_similarity": (
                round(
                    jaccard(
                        signature_set(rows[0]),
                        signature_set(rows[-1]),
                    ),
                    4,
                )
                if len(rows) > 1 else 1.0
            ),
        }
    return result


def arrangement_signature(recipe: dict[str, Any]) -> tuple[tuple[Any, ...], ...]:
    return tuple(
        (
            str(section.get("label", "")),
            int(section.get("start_bar", 0)),
            int(section.get("bars", 0)),
            str(section.get("role", "")),
            str(section.get("relationship", "")),
        )
        for section in recipe.get("native_form", {}).get("sections", [])
    )


def complete_signature(sample: dict[str, Any], normalized: bool) -> str:
    recipe = sample["recipe"]
    value = {
        "harmony": harmony_signature(recipe, normalized),
        "melody": event_signature(
            role_events(recipe, "melody"), normalized, melody=True
        ),
        "bass": event_signature(role_events(recipe, "bass"), normalized),
        "support": event_signature(role_events(recipe, "support"), normalized),
        "drums": drum_signature(recipe, expressive=not normalized),
        "arrangement": arrangement_signature(recipe),
    }
    return stable_digest(value)


def signature_set(signature: Sequence[Sequence[Any]]) -> set[tuple[Any, ...]]:
    return {tuple(item) for item in signature}


def jaccard(left: set[Any], right: set[Any]) -> float:
    union = left | right
    return len(left & right) / len(union) if union else 1.0


def pair_indices(count: int) -> Iterable[tuple[int, int]]:
    return itertools.combinations(range(count), 2)


def count_equal_pairs(values: Sequence[Any]) -> tuple[int, list[tuple[int, int]]]:
    matches = [(left, right) for left, right in pair_indices(len(values))
               if values[left] == values[right]]
    return len(matches), matches


def section_density_changes(
    recipe: dict[str, Any],
) -> tuple[int, list[dict[str, Any]]]:
    sections = recipe.get("native_form", {}).get("sections", [])
    lanes = {
        "melody": role_events(recipe, "melody"),
        "bass": role_events(recipe, "bass"),
        "support": role_events(recipe, "support"),
        "drums": recipe.get("groove", {}).get("performance_events", []),
    }
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    rows: list[dict[str, Any]] = []
    for section in sections:
        start_bar = max(1, int(section.get("start_bar", 1)))
        bars = max(1, int(section.get("bars", 1)))
        start_tick = (start_bar - 1) * beats_per_bar * TICKS_PER_BEAT
        end_tick = (start_bar - 1 + bars) * beats_per_bar * TICKS_PER_BEAT
        density = {
            lane: round(
                sum(
                    start_tick <= int(event.get("tick", 0)) < end_tick
                    for event in events
                ) / bars,
                3,
            )
            for lane, events in lanes.items()
        }
        harmonic_events = recipe.get("progression", {}).get("base_harmony", [])
        start_beat = (start_bar - 1) * beats_per_bar
        end_beat = (start_bar - 1 + bars) * beats_per_bar
        density["harmony"] = round(
            sum(
                start_beat <= int(event.get("beat", 0)) < end_beat
                for event in harmonic_events
            ) / bars,
            3,
        )
        rows.append(
            {
                "label": str(section.get("label", "")),
                "role": str(section.get("role", "")),
                "bars": bars,
                "events_per_bar": density,
            }
        )
    changes = 0
    for left, right in zip(rows, rows[1:]):
        if any(
            abs(left["events_per_bar"][lane] - right["events_per_bar"][lane])
            >= 0.25
            for lane in left["events_per_bar"]
        ):
            changes += 1
    return changes, rows


def active_harmony_at_beat(
    harmony: Sequence[dict[str, Any]],
    beat: int,
) -> dict[str, Any] | None:
    active = None
    for event in harmony:
        if int(event.get("beat", -1)) > beat:
            break
        active = event
    return active


def full_form_evidence(recipe: dict[str, Any]) -> dict[str, Any]:
    """Expose length, boundary, continuity, and ending evidence without scoring it."""
    bars = max(1, int(recipe.get("bars", 1)))
    beats_per_bar = max(1, int(recipe.get("beats_per_bar", 4)))
    bar_ticks = beats_per_bar * TICKS_PER_BEAT
    total_ticks = bars * bar_ticks
    sections = sorted(
        recipe.get("native_form", {}).get("sections", []),
        key=lambda section: int(section.get("start_bar", 1)),
    )
    harmony = final_harmony_events(recipe)
    melody = sorted(
        role_events(recipe, "melody"),
        key=lambda event: int(event.get("tick", 0)),
    )
    bass = role_events(recipe, "bass")
    drums = recipe.get("groove", {}).get("performance_events", [])
    phrase_plan = sorted(
        recipe.get("groove", {}).get("phrase_plan", []),
        key=lambda phrase: int(phrase.get("start_bar", 1)),
    )

    section_coverage = set()
    for section in sections:
        start = int(section.get("start_bar", 1))
        length = int(section.get("bars", 0))
        section_coverage.update(range(start, start + length))
    phrase_coverage = set()
    for phrase in phrase_plan:
        start = int(phrase.get("start_bar", 1))
        end = int(phrase.get("end_bar", start))
        phrase_coverage.update(range(start, end + 1))
    expected = set(range(1, bars + 1))

    boundaries = []
    for previous, following in zip(sections, sections[1:]):
        boundary_bar = int(following.get("start_bar", 1))
        boundary_beat = (boundary_bar - 1) * beats_per_bar
        boundary_tick = boundary_beat * TICKS_PER_BEAT
        prior_harmony = active_harmony_at_beat(
            harmony, max(0, boundary_beat - 1)
        )
        landing_harmony = active_harmony_at_beat(harmony, boundary_beat)
        before = next(
            (
                event for event in reversed(melody)
                if int(event.get("tick", 0)) < boundary_tick
            ),
            None,
        )
        after = next(
            (
                event for event in melody
                if int(event.get("tick", 0)) >= boundary_tick
            ),
            None,
        )
        lead_in_fills = [
            event for event in drums
            if bool(event.get("fill", False))
            and boundary_tick - bar_ticks
            <= int(event.get("tick", 0))
            < boundary_tick
        ]
        landing_accents = [
            event for event in drums
            if int(event.get("tick", -1)) == boundary_tick
            and (
                str(event.get("role", "")) == "section-accent"
                or str(event.get("lane", "")) in {"crash", "ride"}
            )
        ]
        bass_landing = first_event_at_tick(bass, boundary_tick)
        boundaries.append({
            "bar": boundary_bar,
            "from": str(previous.get("label", "")),
            "to": str(following.get("label", "")),
            "to_role": str(following.get("role", "")),
            "prior_chord": (
                str(prior_harmony.get("chord", ""))
                if prior_harmony else ""
            ),
            "landing_chord": (
                str(landing_harmony.get("chord", ""))
                if landing_harmony else ""
            ),
            "harmony_changes": bool(
                prior_harmony
                and landing_harmony
                and str(prior_harmony.get("chord", ""))
                != str(landing_harmony.get("chord", ""))
            ),
            "lead_in_fill_hits": len(lead_in_fills),
            "landing_accent_hits": len(landing_accents),
            "bass_lands_at_boundary": bool(bass_landing),
            "melody_gap_ticks": (
                int(after.get("tick", 0))
                - (
                    int(before.get("tick", 0))
                    + int(before.get("duration_ticks", 0))
                )
                if before and after else -1
            ),
            "melody_boundary_interval": (
                int(after.get("midi", 0))
                - int(before.get("midi", 0))
                if before and after else None
            ),
        })

    final_harmony = active_harmony_at_beat(
        harmony, bars * beats_per_bar - 1
    )
    final_chord = (
        chord_details(str(final_harmony.get("chord", "")))
        if final_harmony else None
    )
    final_melody = melody[-1] if melody else None
    tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
    return {
        "bars": bars,
        "section_count": len(sections),
        "section_coverage_valid": section_coverage == expected,
        "drum_phrase_plan_count": len(phrase_plan),
        "drum_phrase_plan_coverage_valid": (
            phrase_coverage == expected if phrase_plan else False
        ),
        "drum_energy_trajectory": [
            int(phrase.get("energy", 0)) for phrase in phrase_plan
        ],
        "boundaries": boundaries,
        "ending": {
            "final_chord": (
                str(final_harmony.get("chord", ""))
                if final_harmony else ""
            ),
            "final_chord_root_is_tonic": bool(
                final_chord and tonic is not None
                and final_chord["root"] == tonic
            ),
            "final_melody_is_chord_tone": bool(
                final_melody and final_chord
                and int(final_melody.get("midi", -1)) % 12
                in final_chord["pitch_classes"]
            ),
            "final_melody_is_tonic": bool(
                final_melody and tonic is not None
                and int(final_melody.get("midi", -1)) % 12 == tonic
            ),
            "last_bar_fill_hits": sum(
                bool(event.get("fill", False))
                and total_ticks - bar_ticks
                <= int(event.get("tick", 0))
                < total_ticks
                for event in drums
            ),
        },
    }


def onset_overlap(
    left: Sequence[dict[str, Any]],
    right: Sequence[dict[str, Any]],
) -> float:
    left_ticks = {int(event.get("tick", 0)) for event in left}
    right_ticks = {int(event.get("tick", 0)) for event in right}
    return ratio(len(left_ticks & right_ticks), len(left_ticks))


def melody_support_collision(
    melody: Sequence[dict[str, Any]],
    support: Sequence[dict[str, Any]],
) -> float:
    support_notes = {
        (int(event.get("tick", 0)), int(event.get("midi", -1)))
        for event in support
    }
    collisions = sum(
        (int(event.get("tick", 0)), int(event.get("midi", -2)))
        in support_notes
        for event in melody
    )
    return ratio(collisions, len(melody))


def sample_metrics(sample: dict[str, Any]) -> dict[str, Any]:
    recipe = sample["recipe"]
    bars = max(1, int(recipe.get("bars", 1)))
    melody = role_events(recipe, "melody")
    bass = role_events(recipe, "bass")
    support = role_events(recipe, "support")
    drums = recipe.get("groove", {}).get("performance_events", [])
    kick = [event for event in drums if event.get("lane") == "kick"]
    melody_midis = [int(event.get("midi", 0)) for event in melody]
    changes, sections = section_density_changes(recipe)
    chord_events = recipe.get("progression", {}).get("base_harmony", [])
    chord_durations = [
        int(event.get("duration_beats", 0)) for event in chord_events
    ]
    theory_rows = theory_operation_audit(recipe)
    return {
        "bpm": int(recipe.get("bpm", 0)),
        "bars": bars,
        "events_per_bar": {
            "harmony": ratio(len(chord_events), bars),
            "melody": ratio(len(melody), bars),
            "bass": ratio(len(bass), bars),
            "support": ratio(len(support), bars),
            "drums": ratio(len(drums), bars),
        },
        "chord_duration_beats": summarize(chord_durations),
        "melody_pitch_range": (
            max(melody_midis) - min(melody_midis) if melody_midis else 0
        ),
        "section_count": len(sections),
        "section_density_change_count": changes,
        "section_density": sections,
        "kick_bass_onset_overlap": onset_overlap(kick, bass),
        "melody_support_unison_collision": melody_support_collision(
            melody, support
        ),
        "drum_flow": drum_flow_metrics(recipe),
        "melody_recurrence": section_melody_recurrence(recipe),
        "four_bar_flow": four_bar_flow_metrics(recipe),
        "full_form": full_form_evidence(recipe),
        "theory_operations": theory_rows,
    }


def contract_failures(sample: dict[str, Any]) -> list[str]:
    recipe = sample["recipe"]
    profile_id = str(sample.get("profile_id", ""))
    constraints = sample.get("research_constraints", {})
    failures: list[str] = []
    bpm = int(recipe.get("bpm", 0))
    minimum_bpm = int(constraints.get("minimum_bpm", 0))
    maximum_bpm = int(constraints.get("maximum_bpm", 999))
    if not minimum_bpm <= bpm <= maximum_bpm:
        failures.append(f"bpm {bpm} outside {minimum_bpm}..{maximum_bpm}")
    if recipe.get("time", {}).get("meter_id") != sample.get("requested_meter"):
        failures.append("generated meter differs from requested native form")
    if recipe.get("native_form", {}).get("id") != sample.get("form_id"):
        failures.append("generated form id differs from requested form")
    if int(recipe.get("bars", 0)) != int(sample.get("requested_bars", 0)):
        failures.append("generated bar count differs from requested form")
    allowed_progressions = set(constraints.get("progression_families", []))
    progression_family = recipe.get("progression", {}).get("family_id")
    if allowed_progressions and progression_family not in allowed_progressions:
        failures.append(
            f"progression family {progression_family!r} is outside profile contract"
        )
    total_ticks = (
        int(recipe.get("bars", 0))
        * int(recipe.get("beats_per_bar", 0))
        * TICKS_PER_BEAT
    )
    for lane in ("melody", "bass", "support"):
        for event in role_events(recipe, lane):
            tick = int(event.get("tick", -1))
            duration = int(event.get("duration_ticks", 0))
            if tick < 0 or duration <= 0 or tick + duration > total_ticks:
                failures.append(f"{lane} event exceeds the native form")
                break
    if any(
        bool(event.get("fill", False)) and
        str(event.get("lane", "")).endswith("_tom") and
        int(event.get("velocity", 0)) < 28
        for event in recipe.get("groove", {}).get(
            "performance_events", []
        )
    ):
        failures.append(
            "planned tom fill contains an inaudibly low ghost-band hit"
        )
    sections = recipe.get("native_form", {}).get("sections", [])
    covered = set()
    for section in sections:
        start = int(section.get("start_bar", 0))
        length = int(section.get("bars", 0))
        covered.update(range(start, start + length))
    expected = set(range(1, int(recipe.get("bars", 0)) + 1))
    if covered != expected:
        failures.append("form sections do not cover every bar exactly")
    if profile_id.startswith("modal_"):
        tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
        characteristic = MODE_CHARACTERISTIC_INTERVALS.get(
            str(recipe.get("mode", "")).strip().upper()
        )
        melody = role_events(recipe, "melody")
        if (
            tonic is not None and
            characteristic is not None and
            not any(
                int(event.get("midi", -1)) % 12 ==
                (tonic + characteristic) % 12
                for event in melody
            )
        ):
            failures.append(
                "modal melody never states its characteristic degree"
            )
    if profile_id in {"jpop_anisong_rock", "jpop_idol_dance"}:
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        if mode not in {"Major", "Natural Minor"}:
            failures.append(
                "J-Pop mode is outside the researched major/minor families"
            )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        operation_limit = (
            4 if profile_id == "jpop_anisong_rock" else 3
        )
        if len(decisions) > operation_limit:
            failures.append(
                "J-Pop harmonic complexity exceeds the profile operation "
                "budget"
            )
        if complexity <= 1 and decisions:
            failures.append(
                "foundation J-Pop applies chromatic complexity operations"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        bass = role_events(recipe, "bass")
        if len(bass) / bars > 6.5:
            failures.append(
                "J-Pop bass has reverted to a mechanical attack on every "
                "eighth note"
            )

        support = role_events(recipe, "support")
        if (
            profile_id == "jpop_idol_dance" and
            not any(
                event.get("role") == "call_response"
                for event in support
            )
        ):
            failures.append(
                "Idol J-Pop omits its foundational group-response role"
            )

        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 4))
        )
        melody = role_events(recipe, "melody")
        target_labels = (
            {"Return"}
            if profile_id == "jpop_anisong_rock"
            else {"Hook A'"}
        )
        target_section = next(
            (
                section for section in sections
                if str(section.get("label", "")) in target_labels
            ),
            None,
        )
        if target_section is not None and sections:
            opening_start = (
                int(sections[0].get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            target_start = (
                int(target_section.get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            hook_span = 2 * beats_per_bar * TICKS_PER_BEAT

            def relative_hook_onsets(
                start_tick: int,
            ) -> set[int]:
                return {
                    int(event.get("tick", 0)) - start_tick
                    for event in melody
                    if start_tick <= int(event.get("tick", 0))
                    < start_tick + hook_span
                }

            opening_hook = relative_hook_onsets(opening_start)
            recalled_hook = relative_hook_onsets(target_start)
            recall = (
                len(opening_hook & recalled_hook) /
                max(1, len(opening_hook))
            )
            if opening_hook and recall < 0.5:
                failures.append(
                    "J-Pop return does not recall at least half of the "
                    "opening two-bar onset hook"
                )

        tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
        harmony = final_harmony_events(recipe)
        if (
            profile_id == "jpop_anisong_rock" and
            any(
                str(section.get("label", "")) == "Return"
                for section in sections
            ) and
            harmony and
            tonic is not None
        ):
            final_chord = chord_details(
                str(harmony[-1].get("chord", ""))
            )
            if not final_chord or final_chord["root"] != tonic:
                failures.append(
                    "full Anisong return does not close in the home key"
                )

        key_regions = [
            decision for decision in decisions
            if decision.get("kind") == "section-key-region"
        ]
        if key_regions and (
            profile_id != "jpop_anisong_rock" or complexity < 7
        ):
            failures.append(
                "section key region appears outside advanced Anisong"
            )
        for decision in key_regions:
            beat = int(decision.get("beat", -1))
            b_section = next(
                (
                    section for section in sections
                    if str(section.get("label", "")) == "B"
                ),
                None,
            )
            b_start = (
                (int(b_section.get("start_bar", 1)) - 1)
                * beats_per_bar
                if b_section else -1
            )
            if beat != b_start:
                failures.append(
                    "Anisong section key region does not begin at B"
                )
                continue
            at_index = next(
                (
                    index for index, event in enumerate(harmony)
                    if int(event.get("beat", -1)) == beat
                ),
                -1,
            )
            local_tonic = chord_details(
                str(decision.get("after", ""))
            )
            actual = (
                chord_details(str(harmony[at_index]["chord"]))
                if at_index >= 0 else None
            )
            preparation = (
                chord_details(str(harmony[at_index - 1]["chord"]))
                if at_index > 0 else None
            )
            if (
                not local_tonic or
                not actual or
                actual["root"] != local_tonic["root"]
            ):
                failures.append(
                    "Anisong key region does not arrive on its claimed "
                    "local tonic"
                )
            if (
                not local_tonic or
                not preparation or
                (preparation["root"] - local_tonic["root"]) % 12 != 7 or
                "7" not in preparation["suffix"]
            ):
                failures.append(
                    "Anisong key region lacks its prepared dominant arrival"
                )

            return_section = next(
                (
                    section for section in sections
                    if str(section.get("label", "")) == "Return"
                ),
                None,
            )
            if return_section is not None and tonic is not None:
                return_beat = (
                    int(return_section.get("start_bar", 1)) - 1
                ) * beats_per_bar
                return_index = next(
                    (
                        index for index, event in enumerate(harmony)
                        if int(event.get("beat", -1)) == return_beat
                    ),
                    -1,
                )
                home_arrival = (
                    chord_details(
                        str(harmony[return_index]["chord"])
                    )
                    if return_index >= 0 else None
                )
                home_preparation = (
                    chord_details(
                        str(harmony[return_index - 1]["chord"])
                    )
                    if return_index > 0 else None
                )
                if (
                    not home_arrival or
                    home_arrival["root"] != tonic or
                    not home_preparation or
                    (
                        home_preparation["root"] - tonic
                    ) % 12 != 7 or
                    "7" not in home_preparation["suffix"]
                ):
                    failures.append(
                        "Anisong key region does not prepare and return "
                        "to the home key"
                    )

        if tonic is not None:
            for event in recipe.get("progression", {}).get(
                "base_harmony", []
            ):
                if str(event.get("roman", "")) != "V/vii":
                    continue
                chord = chord_details(str(event.get("chord", "")))
                if (
                    not chord or
                    chord["root"] != (tonic + 7) % 12 or
                    chord["bass"] != (tonic + 11) % 12
                ):
                    failures.append(
                        "J-Pop descending-bass V/vii is not realised as "
                        "the dominant in first inversion"
                    )
                    break
    if profile_id in {
        "country_honky_tonk",
        "country_contemporary",
    }:
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        allowed_modes = (
            {"Major", "Mixolydian", "Major Pentatonic"}
            if profile_id == "country_honky_tonk"
            else {"Major", "Natural Minor"}
        )
        if mode not in allowed_modes:
            failures.append(
                "Country mode is outside the profile's researched "
                "collections"
            )

        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        if (
            profile_id == "country_honky_tonk"
            and progression_id == "country-minor-pop"
        ):
            failures.append(
                "traditional Country selected the contemporary minor "
                "progression"
            )
        if (
            profile_id == "country_honky_tonk"
            and progression_id == "country-1v2"
            and mode != "Major"
        ):
            failures.append(
                "traditional applied-dominant route is not in Major"
            )
        if (
            profile_id == "country_contemporary"
            and (
                (mode == "Natural Minor") !=
                (progression_id == "country-minor-pop")
            )
        ):
            failures.append(
                "Contemporary Country minor mode and minor progression "
                "do not agree"
            )

        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        meter_id = str(
            recipe.get("time", {}).get("meter_id", "")
        )
        if (
            meter_id == "3-4"
            and groove_id != "country-waltz"
        ):
            failures.append(
                "Country waltz form does not use the three-beat groove"
            )
        if (
            meter_id == "6-8"
            and groove_id != "country-compound"
        ):
            failures.append(
                "compound Country form does not use the compound groove"
            )
        if (
            meter_id not in {"3-4", "6-8"}
            and groove_id in {
                "country-waltz",
                "country-compound",
            }
        ):
            failures.append(
                "meter-specific Country groove appears in an incompatible "
                "form"
            )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        operation_limit = (
            3 if profile_id == "country_honky_tonk" else 4
        )
        if len(decisions) > operation_limit:
            failures.append(
                "Country harmonic complexity exceeds the profile "
                "operation budget"
            )
        if complexity <= 1 and decisions:
            failures.append(
                "foundation Country applies harmonic complexity "
                "operations"
            )
        allowed_operation_kinds = {
            "country-extension",
            "inversion",
            "modal-interchange",
            "passing-diminished",
            "secondary-dominant",
        }
        if any(
            str(decision.get("kind", ""))
            not in allowed_operation_kinds
            for decision in decisions
        ):
            failures.append(
                "Country selected an out-of-profile harmonic operation"
            )
        country_suffixes = (
            {"6", "7"}
            if profile_id == "country_honky_tonk"
            else {"add9", "m7", "7"}
        )
        for decision in decisions:
            if decision.get("kind") != "country-extension":
                continue
            chord = chord_details(str(decision.get("after", "")))
            if (
                not chord
                or chord["suffix"] not in country_suffixes
            ):
                failures.append(
                    "Country extension uses a generic non-Country chord "
                    "colour"
                )
                break

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bass = role_events(recipe, "bass")
        bass_density = len(bass) / bars
        if bass_density > 3.25:
            failures.append(
                "Country bass has reverted to a mechanical every-beat "
                "pattern"
            )
        if (
            complexity <= 1
            and abs(bass_density - 2.0) > 1e-9
        ):
            failures.append(
                "foundation Country bass does not preserve its two-feel "
                "anchors"
            )
        if complexity <= 1:
            expected_positions = {
                "2-4": {0, 1},
                "3-4": {0, 2},
                "4-4": {0, 2},
                "6-8": {0, 3},
            }.get(meter_id)
            if expected_positions is not None:
                actual_positions = {
                    (
                        int(event.get("tick", 0))
                        // TICKS_PER_BEAT
                    ) % beats_per_bar
                    for event in bass
                }
                if actual_positions != expected_positions:
                    failures.append(
                        "foundation Country bass attacks outside the "
                        "meter-specific two-feel anchors"
                    )
        ordered_bass = sorted(
            bass,
            key=lambda event: int(event.get("tick", 0)),
        )
        for index, event in enumerate(ordered_bass):
            relationship = str(
                event.get("relationship", "")
            )
            if (
                "phrase-edge walk" not in relationship
                and "restrained connecting tone"
                not in relationship
            ):
                continue
            if (
                index + 1 >= len(ordered_bass)
                or abs(
                    int(ordered_bass[index + 1].get("midi", 0))
                    - int(event.get("midi", 0))
                ) > 2
            ):
                failures.append(
                    "Country bass connector does not resolve by step into "
                    "the next written bass target"
                )
                break

        support = role_events(recipe, "support")
        if complexity <= 1 and support:
            failures.append(
                "foundation Country activates an unnecessary support role"
            )
        if complexity >= 3 and not support:
            failures.append(
                "developed Country omits its phrase response"
            )
        if len(support) / bars > 0.6:
            failures.append(
                "Country support role crowds the sung lead"
            )
        if any(
            event.get("role") == "hook_double"
            and (
                profile_id != "country_contemporary"
                or complexity < 6
            )
            for event in support
        ):
            failures.append(
                "Country hook double appears outside advanced "
                "Contemporary Country"
            )
        for event in support:
            relationship = str(
                event.get("relationship", "")
            )
            if "Country phrase" not in relationship:
                continue
            beat = (
                int(event.get("tick", 0))
                // TICKS_PER_BEAT
            )
            if beat % beats_per_bar != beats_per_bar - 1:
                failures.append(
                    "Country instrumental answer is not in the "
                    "four-bar phrase-end space"
                )
                break

        melody = role_events(recipe, "melody")
        if mode == "Mixolydian":
            tonic = tonic_pitch_class(
                str(recipe.get("tonic", ""))
            )
            if (
                tonic is not None
                and not any(
                    int(event.get("midi", -1)) % 12
                    == (tonic + 10) % 12
                    for event in melody
                )
            ):
                failures.append(
                    "Mixolydian Country melody never states the flat "
                    "seventh"
                )

        opening_start = (
            (int(sections[0].get("start_bar", 1)) - 1)
            * beats_per_bar
            * TICKS_PER_BEAT
            if sections else 0
        )
        hook_span = 2 * beats_per_bar * TICKS_PER_BEAT

        def country_call_onsets(start_tick: int) -> set[int]:
            return {
                int(event.get("tick", 0)) - start_tick
                for event in melody
                if start_tick <= int(event.get("tick", 0))
                < start_tick + hook_span
            }

        opening_call = country_call_onsets(opening_start)
        target_sections = [
            section
            for section in sections[1:]
            if (
                (
                    profile_id == "country_honky_tonk"
                    and (
                        "A'" in str(section.get("label", ""))
                        or "Return" in str(
                            section.get("label", "")
                        )
                    )
                )
                or (
                    profile_id == "country_contemporary"
                    and (
                        "arrival" in str(
                            section.get("role", "")
                        ).lower()
                        or "return" in str(
                            section.get("role", "")
                        ).lower()
                    )
                )
            )
        ]
        for section in target_sections:
            target_start = (
                int(section.get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            recalled = country_call_onsets(target_start)
            recall = (
                len(opening_call & recalled)
                / max(1, len(opening_call))
            )
            if opening_call and recall < 0.5:
                failures.append(
                    "Country return does not recall at least half of the "
                    "opening two-bar sung-call rhythm"
                )
                break

        tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
        harmony = final_harmony_events(recipe)
        final_chord = (
            chord_details(str(harmony[-1].get("chord", "")))
            if harmony else None
        )
        if (
            profile_id == "country_honky_tonk"
            and tonic is not None
            and final_chord
            and final_chord["root"]
            not in {tonic, (tonic + 7) % 12}
        ):
            failures.append(
                "traditional Country ending is neither tonic nor an open "
                "dominant route"
            )
        if (
            profile_id == "country_contemporary"
            and any(
                "Return" in str(section.get("label", ""))
                for section in sections
            )
            and tonic is not None
            and (
                not final_chord
                or final_chord["root"] != tonic
            )
        ):
            failures.append(
                "Contemporary Country return does not close on tonic"
            )

        performance = recipe.get("groove", {}).get(
            "performance_events", []
        )
        if groove_id == "country-waltz":
            non_fill_by_tick: dict[int, set[str]] = defaultdict(set)
            for event in performance:
                if not bool(event.get("fill", False)):
                    non_fill_by_tick[int(event.get("tick", 0))].add(
                        str(event.get("lane", ""))
                    )
            if any(
                {"snare", "cross_stick"} <= lanes
                for lanes in non_fill_by_tick.values()
            ):
                failures.append(
                    "Country waltz doubles snare and cross-stick on the "
                    "same pulse"
                )
        expected_fill_prefix = {
            "country-waltz": "country-waltz-",
            "country-compound": "country-compound-",
        }.get(groove_id)
        if expected_fill_prefix and any(
            str(phrase.get("fill_id", ""))
            and not str(phrase.get("fill_id", "")).startswith(
                expected_fill_prefix
            )
            for phrase in recipe.get("groove", {}).get(
                "phrase_plan", []
            )
        ):
            failures.append(
                "meter-specific Country form uses the wrong fill "
                "vocabulary"
            )
    if profile_id in {
        "soul_classic_motown",
        "rnb_contemporary_neosoul",
    }:
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        meter_id = str(
            recipe.get("time", {}).get("meter_id", "")
        )
        allowed_progressions = {
            "soul_classic_motown": {
                "rnb-1625", "rnb-251", "rnb-minor4",
                "rnb-plagal",
            },
            "rnb_contemporary_neosoul": {
                "rnb-1625", "rnb-4362", "rnb-minor4",
                "rnb-backdoor", "rnb-plagal",
            },
        }[profile_id]
        if progression_id not in allowed_progressions:
            failures.append(
                "R&B/Soul progression is outside the profile grammar"
            )
        expected_mode = (
            "Dorian"
            if progression_id == "rnb-minor4"
            else "Major"
        )
        if mode != expected_mode:
            failures.append(
                "R&B/Soul progression and declared tonal collection do "
                "not agree"
            )

        compound_groove = {
            "soul_classic_motown": "soul-12-8",
            "rnb_contemporary_neosoul": "rnb-12-8",
        }[profile_id]
        straight_grooves = {
            "soul_classic_motown": {
                "rnb-motown", "rnb-funk-soul",
            },
            "rnb_contemporary_neosoul": {
                "rnb-laid-back", "rnb-neo-soul",
                "rnb-half-ballad", "rnb-funk-soul",
            },
        }[profile_id]
        if (
            meter_id == "12-8" and
            groove_id != compound_groove
        ):
            failures.append(
                "compound R&B/Soul form does not use its authored 12/8 "
                "groove"
            )
        if (
            meter_id != "12-8" and
            groove_id not in straight_grooves
        ):
            failures.append(
                "straight R&B/Soul form uses an incompatible groove"
            )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        operation_limit = (
            0 if complexity <= 1
            else 1 if complexity < 7
            else 2
        )
        if len(decisions) > operation_limit:
            failures.append(
                "R&B/Soul harmonic development exceeds its bounded "
                "operation budget"
            )
        allowed_operation_kinds = {
            "inversion",
            "modal-interchange",
            "secondary-dominant",
            "diatonic-extension",
        }
        if any(
            str(decision.get("kind", ""))
            not in allowed_operation_kinds
            for decision in decisions
        ):
            failures.append(
                "R&B/Soul selected a generic harmonic operation outside "
                "its voice-leading plan"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        melody_density = len(melody) / bars
        compound = meter_id == "12-8"
        melody_limit = (
            (4.25 if compound else 3.6)
            if profile_id == "soul_classic_motown"
            else (3.5 if compound else 3.0)
        )
        if melody_density > melody_limit:
            failures.append(
                "R&B/Soul vocal-like lead has become a continuous "
                "instrumental line"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        allowed_support_roles = {
            "soul_classic_motown": {
                "call_response", "lead_harmony", "horn_stab",
            },
            "rnb_contemporary_neosoul": {
                "lead_harmony", "countermelody", "pad",
            },
        }[profile_id]
        if not support_roles <= allowed_support_roles:
            failures.append(
                "R&B/Soul support lane uses a role outside its ensemble "
                "grammar"
            )
        support_density = len(support) / bars
        support_limit = (
            0.5 if complexity <= 1
            else 0.65 if complexity < 7
            else 1.0
        )
        if (
            profile_id == "rnb_contemporary_neosoul"
            and complexity <= 1 and support
        ):
            failures.append(
                "foundation Neo-Soul activates a support layer before the "
                "sparse lead/bass pocket is established"
            )
        elif support_density > support_limit:
            failures.append(
                "R&B/Soul ensemble responses have become a continuous "
                "second lead"
            )
        if (
            profile_id == "soul_classic_motown"
            and not (
                {"call_response", "horn_stab"}
                & support_roles
            )
        ):
            failures.append(
                "Classic Soul omits its foundational four-bar band "
                "response"
            )
        if (
            profile_id == "soul_classic_motown"
            and complexity >= 7
            and "horn_stab" not in support_roles
        ):
            failures.append(
                "advanced Classic Soul omits its section-ending horn "
                "answer"
            )
        if (
            profile_id == "rnb_contemporary_neosoul"
            and complexity >= 7
            and not {"pad", "countermelody"} <= support_roles
        ):
            failures.append(
                "advanced Neo-Soul does not coordinate its B-section pad "
                "and inner-line answer"
            )

        bass_density = len(bass) / bars
        if (
            profile_id == "soul_classic_motown"
            and not 3.5 <= bass_density <= 5.25
        ):
            failures.append(
                "Classic Soul bass has lost its melodic four-pulse "
                "foundation"
            )
        if (
            profile_id == "rnb_contemporary_neosoul"
            and bass_density > 2.0
        ):
            failures.append(
                "Neo-Soul bass has lost the long spaces beneath its upper "
                "structure"
            )
        if (
            profile_id == "rnb_contemporary_neosoul"
            and complexity >= 3
            and not any(
                "semitone pickup" in str(
                    event.get("relationship", "")
                )
                for event in bass
            )
        ):
            failures.append(
                "developed Neo-Soul omits its sparse resolving bass "
                "pickup"
            )

        target_labels = (
            {"Return"}
            if profile_id == "soul_classic_motown"
            else {"Return", "A'"}
        )
        target_section = next(
            (
                section for section in reversed(sections)
                if str(section.get("label", ""))
                in target_labels
            ),
            None,
        )
        if target_section is not None and sections:
            opening_start = (
                int(sections[0].get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            target_start = (
                int(target_section.get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            call_span = 2 * beats_per_bar * TICKS_PER_BEAT

            def relative_soul_call_onsets(
                start_tick: int,
            ) -> set[int]:
                return {
                    int(event.get("tick", 0)) - start_tick
                    for event in melody
                    if start_tick <= int(event.get("tick", 0))
                    < start_tick + call_span
                }

            opening_call = relative_soul_call_onsets(
                opening_start
            )
            recalled_call = relative_soul_call_onsets(
                target_start
            )
            recall = ratio(
                len(opening_call & recalled_call),
                len(opening_call),
            )
            if opening_call and recall < 0.5:
                failures.append(
                    "R&B/Soul return does not recall at least half of the "
                    "opening two-bar vocal-call rhythm"
                )
    if profile_id == "funk_static_pocket":
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        form_id = str(
            recipe.get("native_form", {}).get("id", "")
        )
        allowed_progressions = {
            "funk-static-1",
            "funk-static-minor",
            "funk-14",
            "funk-minor-b7",
        }
        if progression_id not in allowed_progressions:
            failures.append(
                "Funk progression is outside the static/two-centre vamp "
                "grammar"
            )
        expected_mode = (
            "Dorian"
            if progression_id in {
                "funk-static-minor", "funk-minor-b7",
            }
            else "Mixolydian"
        )
        if mode != expected_mode:
            failures.append(
                "Funk vamp and declared tonal collection do not agree"
            )
        if (
            form_id == "funk-minor-10" and
            progression_id not in {
                "funk-static-minor", "funk-minor-b7",
            }
        ):
            failures.append(
                "minor Funk exchange form uses a major-centred vamp"
            )
        if groove_id not in {
            "funk-the-one",
            "funk-sync-pocket",
            "funk-linear",
            "funk-disco",
            "funk-half-time",
        }:
            failures.append(
                "Funk groove is outside the authored pocket families"
            )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        if decisions:
            failures.append(
                "Funk complexity adds chord operations instead of "
                "developing the interlock"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bar_ticks = beats_per_bar * TICKS_PER_BEAT
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        if len(melody) / bars > 4.25:
            failures.append(
                "Funk short riff has become a continuous lead line"
            )
        bass_density = len(bass) / bars
        if not 3.5 <= bass_density <= 4.1:
            failures.append(
                "Funk bass has lost its root/fifth syncopated cell"
            )
        has_chromatic_pickup = any(
            "chromatic pickup" in str(
                event.get("relationship", "")
            )
            for event in bass
        )
        if complexity <= 1 and has_chromatic_pickup:
            failures.append(
                "foundation Funk introduces the developed chromatic pickup"
            )
        if complexity >= 3 and not has_chromatic_pickup:
            failures.append(
                "developed Funk omits its resolving chromatic pickup"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        if not support_roles <= {
            "horn_stab", "call_response",
        }:
            failures.append(
                "Funk support lane uses a role outside its bounded horn/"
                "ensemble-answer grammar"
            )
        if complexity <= 1 and support:
            failures.append(
                "foundation Funk activates horn answers before the pocket "
                "is established"
            )
        if len(support) / bars > 0.6:
            failures.append(
                "Funk horn responses have become a continuous second lead"
            )
        if complexity >= 3 and "horn_stab" not in support_roles:
            failures.append(
                "developed Funk omits its short horn response"
            )
        if complexity >= 7 and "call_response" not in support_roles:
            failures.append(
                "advanced Funk omits its break/re-entry ensemble answer"
            )

        cycle_ticks = 2 * bar_ticks
        opening_onsets = {
            int(event.get("tick", 0))
            for event in melody
            if 0 <= int(event.get("tick", 0)) < cycle_ticks
        }
        for cycle_start in range(
            cycle_ticks, bars * bar_ticks, cycle_ticks
        ):
            recalled_onsets = {
                int(event.get("tick", 0)) - cycle_start
                for event in melody
                if cycle_start <= int(event.get("tick", 0))
                < min(cycle_start + cycle_ticks, bars * bar_ticks)
            }
            recall = ratio(
                len(opening_onsets & recalled_onsets),
                len(opening_onsets),
            )
            if opening_onsets and recall < 0.6:
                failures.append(
                    "Funk later state does not recall the opening two-bar "
                    "riff attack map"
                )
                break

        target_section = next(
            (
                section for section in sections
                if str(section.get("label", ""))
                in {"Break B", "Stop / Return"}
            ),
            None,
        )
        if complexity >= 3 and target_section is not None:
            start_bar = int(target_section.get("start_bar", 1))
            section_bars = int(target_section.get("bars", 0))
            bass_ticks = {
                int(event.get("tick", -1))
                for event in bass
            }
            if all(
                (bar - 1) * bar_ticks in bass_ticks
                for bar in range(
                    start_bar,
                    start_bar + section_bars,
                )
            ):
                failures.append(
                    "developed Funk break never subtracts a bass downbeat"
                )

        kick_ticks = {
            int(event.get("tick", -1))
            for event in recipe.get("groove", {}).get(
                "performance_events", []
            )
            if event.get("lane") == "kick"
        }
        kick_ones = sum(
            (bar - 1) * bar_ticks in kick_ticks
            for bar in range(1, bars + 1)
        )
        if ratio(kick_ones, bars) < 0.75:
            failures.append(
                "Funk drummer no longer anchors at least three quarters "
                "of bars on the one"
            )
    if profile_id == "reggae_roots":
        complexity = int(recipe.get("complexity", 0))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        expected_modes = {
            "reggae-1451": "Major",
            "reggae-1564": "Major",
            "reggae-minor": "Natural Minor",
            "reggae-mixolydian": "Mixolydian",
        }
        if progression_id not in expected_modes:
            failures.append(
                "Roots Reggae progression is outside its authored riddim "
                "grammar"
            )
        elif str(recipe.get("mode", "")) != expected_modes[progression_id]:
            failures.append(
                "Roots Reggae progression and declared tonal collection "
                "do not agree"
            )

        form_id = str(
            recipe.get("native_form", {}).get("id", "")
        )
        if form_id not in {
            "reggae-steppers-12",
            "reggae-16",
            "reggae-24",
        }:
            failures.append(
                "Roots Reggae generated an incompatible native form"
            )
        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        if groove_id not in {
            "reggae-one-drop",
            "reggae-rockers",
            "reggae-steppers",
        }:
            failures.append(
                "Roots Reggae generated an incompatible drummer backbone"
            )
        if (
            form_id == "reggae-steppers-12"
            and groove_id != "reggae-steppers"
        ):
            failures.append(
                "twelve-bar steppers form does not use the steppers "
                "backbone"
            )
        if recipe.get("progression", {}).get(
            "theory_decisions", []
        ):
            failures.append(
                "Roots Reggae complexity adds chord operations instead of "
                "developing bass, response, and dub form"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bar_ticks = beats_per_bar * TICKS_PER_BEAT
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        if len(melody) / bars > 2.25:
            failures.append(
                "Roots Reggae vocal-like call has become a continuous "
                "instrumental lead"
            )
        melody_midis = [
            int(event.get("midi", 0))
            for event in melody
        ]
        if (
            melody_midis
            and max(melody_midis) - min(melody_midis) > 19
        ):
            failures.append(
                "Roots Reggae vocal-like call exceeds its restrained "
                "tessitura"
            )

        bass_density = len(bass) / bars
        if complexity <= 1 and abs(bass_density - 2.0) > 1e-9:
            failures.append(
                "foundation Roots Reggae bass does not preserve its "
                "two-note-per-bar riddim cell"
            )
        if complexity >= 3 and not 2.0 < bass_density <= 2.3:
            failures.append(
                "developed Roots Reggae bass has lost its bounded pickup "
                "grammar"
            )
        has_pickup = any(
            "semitone pickup" in str(
                event.get("relationship", "")
            )
            for event in bass
        )
        if complexity <= 1 and has_pickup:
            failures.append(
                "foundation Roots Reggae introduces the developed bass "
                "pickup"
            )
        if complexity >= 3 and not has_pickup:
            failures.append(
                "developed Roots Reggae omits its four-bar bass pickup"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        if not support_roles <= {
            "support_comping",
            "call_response",
            "countermelody",
        }:
            failures.append(
                "Roots Reggae support lane uses a role outside skank/"
                "bubble and bounded-response grammar"
            )
        if "support_comping" not in support_roles:
            failures.append(
                "Roots Reggae omits its complementary organ-bubble role"
            )
        if not 1.6 <= len(support) / bars <= 2.1:
            failures.append(
                "Roots Reggae organ bubble and responses no longer leave "
                "the intended upper-part space"
            )
        if complexity <= 1 and support_roles != {"support_comping"}:
            failures.append(
                "foundation Roots Reggae activates a response before the "
                "riddim is established"
            )
        if complexity >= 3 and "call_response" not in support_roles:
            failures.append(
                "developed Roots Reggae omits its short instrumental "
                "answer"
            )
        if complexity >= 7 and "countermelody" not in support_roles:
            failures.append(
                "advanced Roots Reggae omits its final dub/return response"
            )

        cycle_ticks = 4 * bar_ticks
        opening_onsets = {
            int(event.get("tick", 0))
            for event in melody
            if 0 <= int(event.get("tick", 0)) < cycle_ticks
        }
        for cycle_start in range(
            cycle_ticks, bars * bar_ticks, cycle_ticks
        ):
            recalled_onsets = {
                int(event.get("tick", 0)) - cycle_start
                for event in melody
                if cycle_start <= int(event.get("tick", 0))
                < min(cycle_start + cycle_ticks, bars * bar_ticks)
            }
            allowed_missing = 3
            if (
                opening_onsets
                and (
                    len(opening_onsets & recalled_onsets)
                    < max(1, len(opening_onsets) - allowed_missing)
                    or len(opening_onsets ^ recalled_onsets) > 4
                )
            ):
                failures.append(
                    "Roots Reggae later state no longer performs a bounded "
                    "recall of the opening vocal-call rhythm"
                )
                break

        chord_voicings = sample.get("chord_voicings", [])
        if any(
            str(event.get("articulation", "")) == "short-offbeat"
            and int(event.get("tick", -1)) % TICKS_PER_BEAT
            != TICKS_PER_BEAT // 2
            for event in chord_voicings
        ):
            failures.append(
                "Roots Reggae skank no longer attacks on the eighth-note "
                "offbeat"
            )

        if groove_id == "reggae-one-drop":
            drums = recipe.get("groove", {}).get(
                "performance_events", []
            )
            first_bar_kicks = {
                int(event.get("tick", -1))
                for event in drums
                if event.get("lane") == "kick"
                and 0 <= int(event.get("tick", -1)) < bar_ticks
            }
            if 0 in first_bar_kicks:
                failures.append(
                    "one-drop backbone fills the defining beat-one kick "
                    "space"
                )
            if 2 * TICKS_PER_BEAT not in first_bar_kicks:
                failures.append(
                    "one-drop backbone omits its central beat-three kick"
                )
    if profile_id == "bossa_songbook":
        complexity = int(recipe.get("complexity", 0))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        expected_modes = {
            "bossa-2516": "Major",
            "bossa-1625": "Major",
            "bossa-minor": "Natural Minor",
            "bossa-backdoor": "Major",
            "bossa-cycle": "Major",
        }
        if progression_id not in expected_modes:
            failures.append(
                "Bossa progression is outside its authored songbook "
                "grammar"
            )
        elif str(recipe.get("mode", "")) != expected_modes[progression_id]:
            failures.append(
                "Bossa progression and declared tonal collection do not "
                "agree"
            )

        form_id = str(
            recipe.get("native_form", {}).get("id", "")
        )
        if form_id not in {
            "bossa-aaba-32",
            "bossa-abac-32",
            "bossa-18",
        }:
            failures.append(
                "Bossa generated an incompatible native form"
            )
        if str(recipe.get("groove", {}).get("id", "")) not in {
            "bossa-core", "bossa-sparse",
        }:
            failures.append(
                "Bossa generated an incompatible percussion backbone"
            )
        if form_id == "bossa-18":
            observed = [
                (
                    int(section.get("start_bar", 0)),
                    int(section.get("bars", 0)),
                )
                for section in sections
            ]
            if observed != [(1, 5), (6, 5), (11, 4), (15, 4)]:
                failures.append(
                    "asymmetric Bossa form no longer follows its researched "
                    "5+5+4+4 phrase arc"
                )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        operation_limit = (
            0 if complexity <= 1
            else 1 if complexity < 7
            else 2
        )
        if len(decisions) > operation_limit:
            failures.append(
                "Bossa harmonic development exceeds its bounded "
                "voice-leading operation budget"
            )
        if any(
            str(decision.get("kind", "")) not in {
                "inversion",
                "backdoor-dominant",
                "passing-diminished",
                "secondary-dominant",
                "tritone-substitution",
            }
            for decision in decisions
        ):
            failures.append(
                "Bossa selected a harmonic operation outside its authored "
                "songbook colour grammar"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bar_ticks = beats_per_bar * TICKS_PER_BEAT
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        melody_density = len(melody) / bars
        if not 0.75 <= melody_density <= 2.1:
            failures.append(
                "Bossa vocal-like lead no longer preserves its restrained "
                "speech-like phrase density"
            )
        melody_midis = [
            int(event.get("midi", 0))
            for event in melody
        ]
        if (
            melody_midis and
            (
                min(melody_midis) < 57 or
                max(melody_midis) > 77 or
                max(melody_midis) - min(melody_midis) > 20
            )
        ):
            failures.append(
                "Bossa vocal-like lead exceeds its restrained tessitura"
            )

        bass_density = len(bass) / bars
        if abs(bass_density - 2.0) > 1e-9:
            failures.append(
                "Bossa bass has lost its two-pulse root/answer foundation"
            )
        delayed_answers = sum(
            int(event.get("tick", -1)) % bar_ticks ==
            TICKS_PER_BEAT + TICKS_PER_BEAT // 2
            for event in bass
        )
        if ratio(delayed_answers, bars) < 0.9:
            failures.append(
                "Bossa bass answer no longer sits independently between "
                "the structural pulse and upper syncopation"
            )
        has_phrase_approach = any(
            "chromatic bass approach" in str(
                event.get("relationship", "")
            )
            for event in bass
        )
        if complexity <= 1 and has_phrase_approach:
            failures.append(
                "foundation Bossa introduces the developed bass approach"
            )
        if complexity >= 3 and not has_phrase_approach:
            failures.append(
                "developed Bossa omits its phrase-edge resolving bass "
                "approach"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        if not support_roles <= {
            "support_comping", "countermelody",
        }:
            failures.append(
                "Bossa support lane uses a role outside its sparse "
                "guide-tone/counterline grammar"
            )
        if complexity <= 1 and support:
            failures.append(
                "foundation Bossa activates support before bass, comp, and "
                "lead establish the songbook texture"
            )
        if len(support) / bars > 0.25:
            failures.append(
                "Bossa support has become a continuous second "
                "accompaniment"
            )
        if complexity >= 3 and "support_comping" not in support_roles:
            failures.append(
                "developed Bossa omits its bounded inner guide-tone colour"
            )
        if complexity >= 7 and "countermelody" not in support_roles:
            failures.append(
                "advanced Bossa omits its final lead-breath counterline"
            )

        chord_voicings = sample.get("chord_voicings", [])
        bossa_voicings = [
            event for event in chord_voicings
            if str(event.get("articulation", "")) ==
            "soft-detached"
        ]
        if not bossa_voicings:
            failures.append(
                "Bossa omits its syncopated upper-voicing part"
            )
        if any(
            int(event.get("tick", -1)) % TICKS_PER_BEAT == 0
            for event in bossa_voicings
        ):
            failures.append(
                "Bossa upper voicing has collapsed onto the bass pulse"
            )
        if any(
            int(event.get("tick", -1)) %
            (TICKS_PER_BEAT // 4) != 0
            for event in bossa_voicings
        ):
            failures.append(
                "Bossa upper voicing leaves the researched binary "
                "sixteenth-note grid"
            )

        target_section = next(
            (
                section for section in sections[1:]
                if (
                    "A'" in str(section.get("label", "")) or
                    "Return" in str(section.get("label", "")) or
                    "Tag" in str(section.get("label", ""))
                )
            ),
            None,
        )
        if target_section is not None and sections:
            opening_start = (
                int(sections[0].get("start_bar", 1)) - 1
            ) * bar_ticks
            target_start = (
                int(target_section.get("start_bar", 1)) - 1
            ) * bar_ticks
            call_span = 2 * bar_ticks

            def relative_bossa_call_onsets(
                start_tick: int,
            ) -> set[int]:
                return {
                    int(event.get("tick", 0)) - start_tick
                    for event in melody
                    if start_tick <= int(event.get("tick", 0))
                    < start_tick + call_span
                }

            opening_call = relative_bossa_call_onsets(
                opening_start
            )
            recalled_call = relative_bossa_call_onsets(
                target_start
            )
            if (
                len(opening_call) > 1 and
                ratio(
                    len(opening_call & recalled_call),
                    len(opening_call),
                ) < 0.5
            ):
                failures.append(
                    "Bossa theme return does not recall at least half of "
                    "the opening two-bar vocal attack map"
                )
    if profile_id == "metal_modern_progressive":
        complexity = int(recipe.get("complexity", 0))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        expected_modes = {
            "metal-pedal-phrygian": "Phrygian",
            "metal-aeolian-roots": "Natural Minor",
            "metal-clean-contrast": "Natural Minor",
            "metal-chromatic": "Phrygian",
        }
        if progression_id not in expected_modes:
            failures.append(
                "Modern Metal progression is outside its narrow authored "
                "pedal/colour grammar"
            )
        elif str(recipe.get("mode", "")) != expected_modes[progression_id]:
            failures.append(
                "Modern Metal progression and declared tonal collection "
                "do not agree"
            )

        form_id = str(
            recipe.get("native_form", {}).get("id", "")
        )
        expected_form = {
            "metal-riff-12": [(1, 4), (5, 4), (9, 4)],
            "metal-contrast-18": [(1, 6), (7, 8), (15, 4)],
        }
        observed_form = [
            (
                int(section.get("start_bar", 0)),
                int(section.get("bars", 0)),
            )
            for section in sections
        ]
        if form_id not in expected_form:
            failures.append(
                "Modern Metal generated an incompatible native form"
            )
        elif observed_form != expected_form[form_id]:
            failures.append(
                "Modern Metal form no longer follows its researched "
                "heavy/contrast/return arc"
            )
        meter_id = str(
            recipe.get("time", {}).get("meter_id", "")
        )
        expected_meter = (
            "4-4"
            if form_id == "metal-riff-12"
            else "9-8"
        )
        if meter_id != expected_meter:
            failures.append(
                "Modern Metal form no longer uses its authored written "
                "meter"
            )
        if str(recipe.get("groove", {}).get("id", "")) not in {
            "metal-grouped", "metal-half-time",
        }:
            failures.append(
                "Modern Metal primary groove is not a grouped or half-time "
                "heavy backbone"
            )
        if recipe.get("progression", {}).get(
            "theory_decisions", []
        ):
            failures.append(
                "Modern Metal complexity adds unrelated chord operations "
                "instead of developing articulation and form"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bar_ticks = beats_per_bar * TICKS_PER_BEAT
        melody = sorted(
            role_events(recipe, "melody"),
            key=lambda event: int(event.get("tick", 0)),
        )
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        melody_density = len(melody) / bars
        if not 0.65 <= melody_density <= 2.5:
            failures.append(
                "Modern Metal lead no longer leaves heavy-riff space or "
                "provide a sufficient clean/open phrase"
            )
        melody_midis = [
            int(event.get("midi", 0))
            for event in melody
        ]
        if (
            melody_midis and
            (
                min(melody_midis) < 55 or
                max(melody_midis) > 79
            )
        ):
            failures.append(
                "Modern Metal clean-vocal/lead analogue exceeds its "
                "authored tessitura"
            )
        if any(
            abs(right - left) > 7
            for left, right in zip(
                melody_midis,
                melody_midis[1:],
            )
        ):
            failures.append(
                "Modern Metal clean-vocal/lead analogue exceeds its "
                "singable leap bound"
            )

        def section_density(section: dict[str, Any]) -> float:
            start = (
                int(section.get("start_bar", 1)) - 1
            ) * bar_ticks
            section_bars = max(
                1, int(section.get("bars", 1))
            )
            end = start + section_bars * bar_ticks
            return ratio(
                sum(
                    start <= int(event.get("tick", 0)) < end
                    for event in melody
                ),
                section_bars,
            )

        contrast_sections = [
            section for section in sections
            if (
                "clean" in str(
                    section.get("role", "")
                ).lower() or
                "open" in str(
                    section.get("role", "")
                ).lower()
            )
        ]
        heavy_sections = [
            section for section in sections
            if section not in contrast_sections
        ]
        if contrast_sections and heavy_sections:
            contrast_density = sum(
                section_density(section)
                for section in contrast_sections
            ) / len(contrast_sections)
            heavy_density = sum(
                section_density(section)
                for section in heavy_sections
            ) / len(heavy_sections)
            if contrast_density <= heavy_density:
                failures.append(
                    "Modern Metal clean/open lead does not become more "
                    "prominent than the sparse heavy-section hook"
                )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        if not support_roles <= {
            "pad", "hook_double", "countermelody",
        }:
            failures.append(
                "Modern Metal support lane uses a role outside its sparse "
                "ambient/double/counterline grammar"
            )
        if len(support) / bars > 0.4:
            failures.append(
                "Modern Metal support has become a continuous second "
                "guitar or lead"
            )
        if (
            form_id == "metal-contrast-18" and
            "pad" not in support_roles
        ):
            failures.append(
                "Heavy/clean Metal form omits its restrained ambient clean "
                "layer"
            )
        if complexity <= 1 and not support_roles <= {"pad"}:
            failures.append(
                "foundation Modern Metal activates developed riff doubles "
                "or counterlines"
            )
        if complexity >= 3 and "hook_double" not in support_roles:
            failures.append(
                "developed Modern Metal omits its bounded displaced/return "
                "riff double"
            )
        if complexity >= 7 and "countermelody" not in support_roles:
            failures.append(
                "advanced Modern Metal omits its final clean/open "
                "counterline"
            )

        chord_voicings = sample.get("chord_voicings", [])
        heavy_voicings = [
            event for event in chord_voicings
            if str(event.get("articulation", "")) in {
                "palm-muted",
                "open-accent",
                "gated-choke",
            }
        ]
        open_voicings = [
            event for event in chord_voicings
            if str(event.get("articulation", "")) ==
            "open-sustain"
        ]
        if not heavy_voicings or not open_voicings:
            failures.append(
                "Modern Metal no longer articulates both heavy and "
                "clean/open chord-riff states"
            )
        kick_ticks = {
            int(event.get("tick", -1))
            for event in recipe.get("groove", {}).get(
                "performance_events", []
            )
            if event.get("lane") == "kick"
        }
        if ratio(
            sum(
                int(event.get("tick", -1)) in kick_ticks
                for event in heavy_voicings
            ),
            len(heavy_voicings),
        ) < 0.9:
            failures.append(
                "Modern Metal heavy chord riff no longer locks its attacks "
                "to the authored kick pattern"
            )
        bass_ticks = {
            int(event.get("tick", -1))
            for event in bass
        }
        if any(
            int(event.get("tick", -1)) not in bass_ticks
            for event in chord_voicings
        ):
            failures.append(
                "Modern Metal split bass no longer reinforces every "
                "authored guitar attack"
            )
        if form_id == "metal-contrast-18" and sections:
            final_section = sections[-1]
            final_bar_tick = (
                int(final_section.get("start_bar", 1)) +
                int(final_section.get("bars", 1)) - 2
            ) * bar_ticks
            final_bar_end = final_bar_tick + bar_ticks
            final_kicks = {
                tick for tick in kick_ticks
                if final_bar_tick <= tick < final_bar_end
            }
            final_chords = {
                int(event.get("tick", -1))
                for event in heavy_voicings
                if final_bar_tick <=
                int(event.get("tick", -1)) <
                final_bar_end
            }
            if not final_kicks - final_chords:
                failures.append(
                    "Modern Metal compressed return omits its final-module "
                    "attack subtraction"
                )
    if profile_id in {
        "hiphop_boom_bap",
        "hiphop_trap",
    }:
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        form_id = str(
            recipe.get("native_form", {}).get("id", "")
        )
        boom_bap = profile_id == "hiphop_boom_bap"
        allowed_progressions = (
            {
                "boombap-static-minor",
                "boombap-oscillating-minor",
                "boombap-minor-cell",
                "boombap-major-cell",
                "boombap-dorian-cell",
            }
            if boom_bap
            else {
                "trap-static-minor",
                "trap-oscillating-b6",
                "trap-1b6b7",
                "trap-1b3b74",
                "trap-14b65",
                "trap-descent",
                "trap-phrygian",
                "trap-sparse",
            }
        )
        if progression_id not in allowed_progressions:
            failures.append(
                "Hip-Hop/Trap progression is outside its authored beat-"
                "phrase grammar"
            )
        if boom_bap:
            expected_mode = (
                "Dorian"
                if progression_id == "boombap-dorian-cell"
                else "Major"
                if progression_id == "boombap-major-cell"
                else "Natural Minor"
            )
        else:
            expected_mode = (
                "Phrygian"
                if progression_id == "trap-phrygian"
                else "Natural Minor"
            )
        if mode != expected_mode:
            failures.append(
                "Hip-Hop/Trap beat phrase and declared tonal collection "
                "do not agree"
            )

        allowed_forms = (
            {
                "boombap-12",
                "boombap-16",
                "boombap-odd-15",
            }
            if boom_bap
            else {"trap-12", "trap-16", "trap-24"}
        )
        if form_id not in allowed_forms:
            failures.append(
                "Hip-Hop/Trap generated an incompatible native form"
            )
        allowed_grooves = (
            {"hiphop-boom-bap", "hiphop-lofi"}
            if boom_bap
            else {
                "trap-sparse",
                "trap-rolling",
                "trap-half-time-808",
            }
        )
        if groove_id not in allowed_grooves:
            failures.append(
                "Hip-Hop/Trap generated an incompatible drum backbone"
            )
        if recipe.get("progression", {}).get(
            "theory_decisions", []
        ):
            failures.append(
                "Hip-Hop/Trap complexity adds chord operations instead of "
                "developing the beat phrase"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        bar_ticks = beats_per_bar * TICKS_PER_BEAT
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        melody_limit = 2.6 if boom_bap else 2.3
        if len(melody) / bars > melody_limit:
            failures.append(
                "Hip-Hop/Trap instrumental hook crowds the notional rap "
                "space"
            )
        bass_density = len(bass) / bars
        if boom_bap and not 1.7 <= bass_density <= 2.1:
            failures.append(
                "Boom-Bap bass has lost its short root/answer motif"
            )
        if not boom_bap and not 1.2 <= bass_density <= 1.6:
            failures.append(
                "Trap 808 has lost its sparse root/retrigger/slide grammar"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        if not support_roles <= {
            "riff", "hook_double",
        }:
            failures.append(
                "Hip-Hop/Trap support lane uses a role outside its sparse "
                "instrumental-answer grammar"
            )
        if complexity <= 1 and support:
            failures.append(
                "foundation Hip-Hop/Trap activates support before the beat "
                "phrase is established"
            )
        if len(support) / bars > 0.2:
            failures.append(
                "Hip-Hop/Trap support has become a continuous second hook"
            )
        if complexity >= 3 and "riff" not in support_roles:
            failures.append(
                "developed Hip-Hop/Trap omits its one-shot hook answer"
            )
        if complexity >= 7 and "hook_double" not in support_roles:
            failures.append(
                "advanced Hip-Hop/Trap omits its return/beat-switch hook "
                "double"
            )

        cycle_bars = (
            5
            if form_id == "boombap-odd-15"
            else 4
        )
        cycle_ticks = cycle_bars * bar_ticks
        opening_onsets = {
            int(event.get("tick", 0))
            for event in melody
            if 0 <= int(event.get("tick", 0)) < cycle_ticks
        }
        for cycle_start in range(
            cycle_ticks, bars * bar_ticks, cycle_ticks
        ):
            recalled_onsets = {
                int(event.get("tick", 0)) - cycle_start
                for event in melody
                if cycle_start <= int(event.get("tick", 0))
                < min(cycle_start + cycle_ticks, bars * bar_ticks)
            }
            if (
                opening_onsets and
                (len(opening_onsets & recalled_onsets) <
                     max(1, len(opening_onsets) - 1) or
                 len(opening_onsets ^ recalled_onsets) > 2)
            ):
                failures.append(
                    "Hip-Hop/Trap later state no longer performs a bounded "
                    "recut of the opening beat phrase"
                )
                break

        bass_relationships = [
            str(event.get("relationship", ""))
            for event in bass
        ]
        if boom_bap:
            has_pickup = any(
                "chromatic bass pickup" in relationship
                for relationship in bass_relationships
            )
            if complexity <= 1 and has_pickup:
                failures.append(
                    "foundation Boom-Bap introduces the developed loop "
                    "pickup"
                )
            if complexity >= 3 and not has_pickup:
                failures.append(
                    "developed Boom-Bap omits its bounded loop pickup"
                )
        else:
            has_slide = any(
                "semitone 808 approach" in relationship
                for relationship in bass_relationships
            )
            if complexity <= 1 and has_slide:
                failures.append(
                    "foundation Trap introduces the developed 808 slide"
                )
            if complexity >= 3 and not has_slide:
                failures.append(
                    "developed Trap omits its resolving 808 slide"
                )
            if not any(
                "octave retrigger" in relationship
                for relationship in bass_relationships
            ):
                failures.append(
                    "Trap omits the second note of its sparse 808 cell"
                )
            if any(
                not 28 <= int(event.get("midi", -1)) <= 47
                for event in bass
            ):
                failures.append(
                    "Trap 808 leaves its validated low register"
                )
            if not any(
                int(event.get("duration_ticks", 0)) >= 18
                for event in bass
            ):
                failures.append(
                    "Trap 808 never sustains beyond a short bass attack"
                )
            if has_slide and not any(
                event.get("articulation") == "808-slide"
                for event in bass
            ):
                failures.append(
                    "Trap semitone approach is not encoded as an 808 slide"
                )

        drums = recipe.get("groove", {}).get(
            "performance_events", []
        )
        if boom_bap:
            backbeat_bars = {
                int(event.get("tick", -1)) // bar_ticks
                for event in drums
                if event.get("lane") == "snare" and
                int(event.get("tick", -1)) % bar_ticks
                in {TICKS_PER_BEAT, 3 * TICKS_PER_BEAT}
            }
            if ratio(len(backbeat_bars), bars) < 0.75:
                failures.append(
                    "Boom-Bap drummer loses its repeatable backbeat frame"
                )
        else:
            half_time_bars = {
                int(event.get("tick", -1)) // bar_ticks
                for event in drums
                if event.get("lane") == "snare" and
                int(event.get("tick", -1)) % bar_ticks ==
                2 * TICKS_PER_BEAT
            }
            if ratio(len(half_time_bars), bars) < 0.7:
                failures.append(
                    "Trap drummer loses its beat-three half-time snare"
                )
            roll_section = next(
                (
                    section for section in sections
                    if "Roll / Slide" in str(
                        section.get("label", "")
                    )
                ),
                None,
            )
            if roll_section is not None:
                roll_beat = (
                    int(roll_section.get("start_bar", 1)) +
                    int(roll_section.get("bars", 0)) - 1
                ) * bar_ticks - TICKS_PER_BEAT
                roll_hits = sum(
                    event.get("lane") == "closed_hat" and
                    roll_beat <= int(event.get("tick", -1))
                    < roll_beat + TICKS_PER_BEAT
                    for event in drums
                )
                if roll_hits < 4:
                    failures.append(
                        "Trap Roll / Slide state omits its one explicit "
                        "high-rate hat gesture"
                    )
            negative_section = next(
                (
                    section for section in sections
                    if "subtract hats" in str(
                        section.get("role", "")
                    ).lower()
                ),
                None,
            )
            if negative_section is not None:
                start_tick = (
                    int(negative_section.get("start_bar", 1)) - 1
                ) * bar_ticks
                if any(
                    event.get("lane")
                    in {"closed_hat", "open_hat"} and
                    start_tick <= int(event.get("tick", -1))
                    < start_tick + 2 * TICKS_PER_BEAT
                    for event in drums
                ):
                    failures.append(
                        "Trap negative-space state does not remove hats for "
                        "its opening two beats"
                    )
                if not any(
                    event.get("lane") == "kick" and
                    start_tick < int(event.get("tick", -1))
                    < start_tick + TICKS_PER_BEAT
                    for event in drums
                ):
                    failures.append(
                        "Trap beat switch does not re-space its opening kick"
                    )
    if profile_id in {
        "electronic_house",
        "electronic_techno",
        "electronic_breakbeat",
    }:
        complexity = int(recipe.get("complexity", 0))
        mode = str(recipe.get("mode", ""))
        progression_id = str(
            recipe.get("progression", {}).get("id", "")
        )
        groove_id = str(
            recipe.get("groove", {}).get("id", "")
        )
        allowed_modes = {
            "electronic_house": {
                "Major", "Natural Minor", "Dorian",
            },
            "electronic_techno": {
                "Natural Minor", "Dorian", "Phrygian",
            },
            "electronic_breakbeat": {
                "Major", "Natural Minor", "Dorian",
            },
        }[profile_id]
        if mode not in allowed_modes:
            failures.append(
                "Electronic mode is outside the profile's researched "
                "collections"
            )
        allowed_progression_ids = {
            "electronic_house": {
                "edm-6415", "edm-minor-loop", "edm-1564",
                "edm-4156", "house-dorian-vamp",
            },
            "electronic_techno": {
                "edm-pedal", "techno-single-centre",
                "techno-dorian-pedal",
                "techno-phrygian-neighbour",
            },
            "electronic_breakbeat": {
                "edm-6415", "edm-minor-loop", "edm-descent",
                "edm-1564", "edm-pedal",
                "breakbeat-dorian-loop",
            },
        }[profile_id]
        if progression_id not in allowed_progression_ids:
            failures.append(
                "Electronic progression is outside the profile grammar"
            )
        if (
            ("dorian" in progression_id) != (mode == "Dorian")
            or (
                ("phrygian" in progression_id)
                != (mode == "Phrygian")
            )
        ):
            failures.append(
                "Electronic modal progression and declared collection "
                "do not agree"
            )
        allowed_grooves = {
            "electronic_house": {
                "edm-house", "edm-disco-house",
            },
            "electronic_techno": {
                "edm-techno", "edm-techno-polymetric",
            },
            "electronic_breakbeat": {
                "edm-breakbeat", "edm-breakbeat-swung",
            },
        }[profile_id]
        if groove_id not in allowed_grooves:
            failures.append(
                "Electronic groove is outside the profile grammar"
            )

        decisions = recipe.get("progression", {}).get(
            "theory_decisions", []
        )
        if profile_id == "electronic_techno" and decisions:
            failures.append(
                "Techno pitch process is being inflated by generic "
                "harmonic operations"
            )
        operation_limit = (
            0 if complexity <= 1
            else 1 if complexity < 7
            else 3 if profile_id == "electronic_house"
            else 2
        )
        if len(decisions) > operation_limit:
            failures.append(
                "Electronic harmonic colour exceeds its process-boundary "
                "budget"
            )
        if any(
            str(decision.get("kind", ""))
            not in {"inversion", "modal-interchange"}
            for decision in decisions
        ):
            failures.append(
                "Electronic profile selected an unrelated generic "
                "harmonic device"
            )
        phrase_bars = max(
            1,
            int(
                recipe.get("native_form", {}).get(
                    "phrase_bars", 1
                )
            ),
        )
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        if any(
            (
                int(decision.get("beat", 0))
                // beats_per_bar
                + 1
            ) % phrase_bars
            != 0
            for decision in decisions
        ):
            failures.append(
                "Electronic harmonic colour is not attached to a "
                "phrase/process hand-off"
            )

        bars = max(1, int(recipe.get("bars", 1)))
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        melody_density = len(melody) / bars
        density_limit = {
            "electronic_house": 3.5,
            "electronic_techno": 0.85 * beats_per_bar,
            "electronic_breakbeat": 3.75,
        }[profile_id]
        if melody_density > density_limit:
            failures.append(
                "Electronic identity cell has expanded into a continuous "
                "general-purpose lead"
            )

        cycle_ticks = (
            3 * TICKS_PER_BEAT
            if profile_id == "electronic_techno"
            else 2 * beats_per_bar * TICKS_PER_BEAT
        )
        total_ticks = bars * beats_per_bar * TICKS_PER_BEAT

        def electronic_cycle_onsets(
            cycle_start: int,
        ) -> set[int]:
            return {
                int(event.get("tick", 0)) - cycle_start
                for event in melody
                if cycle_start
                <= int(event.get("tick", 0))
                < min(total_ticks, cycle_start + cycle_ticks)
            }

        opening_cell = electronic_cycle_onsets(0)
        if not opening_cell:
            failures.append(
                "Electronic profile has no authored opening identity cell"
            )
        section_start_ticks = {
            (
                int(section.get("start_bar", 1)) - 1
            ) * beats_per_bar * TICKS_PER_BEAT
            for section in recipe.get(
                "native_form", {}
            ).get("sections", [])
        }
        for cycle_start in range(
            cycle_ticks, total_ticks, cycle_ticks
        ):
            if any(
                cycle_start < boundary <
                cycle_start + cycle_ticks
                for boundary in section_start_ticks
            ):
                # A phased cell may straddle a formal boundary. Each side
                # legitimately belongs to a different bounded process state;
                # judge the intact cycles within those states instead.
                continue
            cycle = electronic_cycle_onsets(cycle_start)
            if (
                cycle_start + cycle_ticks <= total_ticks
                and len(opening_cell ^ cycle) > 2
            ):
                failures.append(
                    "Electronic process rerolls the identity-cell rhythm "
                    "instead of applying a bounded mutation"
                )
                break

        tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
        melody_pitch_classes = {
            int(event.get("midi", -1)) % 12
            for event in melody
        }
        if profile_id == "electronic_techno":
            if (
                tonic is not None
                and tonic not in melody_pitch_classes
            ):
                failures.append(
                    "Techno process cell does not retain its tonal centre"
                )
            techno_pitch_limit = 1 if complexity <= 1 else 2
            if len(melody_pitch_classes) > techno_pitch_limit:
                failures.append(
                    "Techno pitch cell exceeds its centre-plus-neighbour "
                    "contract"
                )
            if (
                complexity >= 3
                and len(melody_pitch_classes) < 2
            ):
                failures.append(
                    "Developed Techno never reveals its declared modal "
                    "neighbour"
                )

        bass_density = len(bass) / bars
        if profile_id == "electronic_house":
            if abs(bass_density - 2.0) > 1e-9:
                failures.append(
                    "House bass does not preserve its two offbeat attacks "
                    "per bar"
                )
            if any(
                int(event.get("tick", 0))
                % TICKS_PER_BEAT == 0
                for event in bass
            ):
                failures.append(
                    "House bass masks the quarter kick instead of "
                    "interlocking off the beat"
                )
        elif profile_id == "electronic_techno":
            if abs(bass_density - beats_per_bar) > 1e-9:
                failures.append(
                    "Techno sub pulse does not preserve one attack per "
                    "written beat"
                )
            if any(
                int(event.get("tick", 0))
                % TICKS_PER_BEAT != 0
                for event in bass
            ):
                failures.append(
                    "Techno sub pulse has drifted away from the stable "
                    "quarter grid"
                )
        elif (
            abs(bass_density - 4.0) > 1e-9
            or not any(
                int(event.get("tick", 0))
                % TICKS_PER_BEAT != 0
                for event in bass
            )
        ):
            failures.append(
                "Breakbeat bass does not preserve its syncopated "
                "four-attack answer"
            )

        support_roles = {
            str(event.get("role", ""))
            for event in support
        }
        allowed_support_roles = {
            "electronic_house": {"hook_double", "pad"},
            "electronic_techno": {"drone", "riff", "pad"},
            "electronic_breakbeat": {"hook_double", "pad"},
        }[profile_id]
        if not support_roles <= allowed_support_roles:
            failures.append(
                "Electronic support lane uses a role outside its process "
                "grammar"
            )
        if (
            profile_id == "electronic_techno"
            and (
                ("pad" in support_roles)
                != (complexity >= 7)
            )
        ):
            failures.append(
                "Techno sustained contrast layer does not match the "
                "advanced process tier"
            )
        if (
            complexity <= 1
            and profile_id != "electronic_techno"
            and support
        ):
            failures.append(
                "Foundation Electronic profile activates a non-core "
                "support layer"
            )
        selected_tools = {
            str(tool.get("tool_id", ""))
            for tool in recipe.get("complexity_tools", [])
            if bool(tool.get("selected", False))
        }
        if "planing" in selected_tools:
            failures.append(
                "Electronic complexity claims planing without an authored "
                "planing operation"
            )
        if selected_tools & {
            "anticipation",
            "bass-approach",
            "independent-bass",
        }:
            failures.append(
                "Electronic complexity claims a bass/anticipation device "
                "that the persistent core does not introduce"
            )
        if (
            profile_id == "electronic_techno"
            and complexity >= 3
            and "characteristic-degree" not in selected_tools
        ):
            failures.append(
                "Developed Techno does not expose its modal-neighbour "
                "complexity concept"
            )
        if "extensions" in selected_tools:
            extended_suffix = re.compile(
                r"(?:6|7|9|11|13|add)",
                re.IGNORECASE,
            )
            if not any(
                (
                    chord := chord_details(
                        str(event.get("chord", ""))
                    )
                )
                and extended_suffix.search(
                    str(chord.get("suffix", ""))
                )
                for event in final_harmony_events(recipe)
            ):
                failures.append(
                    "Electronic complexity claims chord extensions that "
                    "are absent from the final harmony"
                )
        if (
            complexity >= 5
            and "riff-mutation" not in selected_tools
        ):
            failures.append(
                "Advanced Electronic complexity does not expose its "
                "bounded identity-cell mutation"
            )

        performance = recipe.get("groove", {}).get(
            "performance_events", []
        )
        fill_beats = {
            int(event.get("tick", 0)) // TICKS_PER_BEAT
            for event in performance
            if bool(event.get("fill", False))
        }
        kick_ticks = {
            int(event.get("tick", 0))
            for event in performance
            if event.get("lane") == "kick"
            and not bool(event.get("fill", False))
        }
        quarter_ticks = {
            beat * TICKS_PER_BEAT
            for beat in range(bars * beats_per_bar)
            if beat not in fill_beats
        }
        quarter_coverage = ratio(
            len(kick_ticks & quarter_ticks),
            len(quarter_ticks),
        )
        if (
            profile_id in {
                "electronic_house", "electronic_techno",
            }
            and quarter_coverage < 0.82
        ):
            failures.append(
                "Four-floor Electronic kick loses too much of its stable "
                "quarter-note core outside transitions"
            )
        if profile_id == "electronic_breakbeat":
            offbeat_kicks = {
                tick for tick in kick_ticks
                if tick % TICKS_PER_BEAT != 0
            }
            if quarter_coverage > 0.8 or not offbeat_kicks:
                failures.append(
                    "Breakbeat kick has collapsed into a four-floor "
                    "skeleton"
                )
        maximum_timing_offset = (
            6 if profile_id == "electronic_house" else 4
        )
        if profile_id in {
            "electronic_house", "electronic_techno",
        } and any(
            abs(int(event.get("offset_ms", 0)))
            > maximum_timing_offset
            for event in performance
        ):
            failures.append(
                "Machine-led Electronic timing exceeds its bounded "
                "microtiming contract"
            )
    if profile_id in {"blues_dominant", "blues_minor"}:
        tonic = tonic_pitch_class(str(recipe.get("tonic", "")))
        melody = role_events(recipe, "melody")
        relative_pitch_classes = {
            (int(event.get("midi", -1)) - tonic) % 12
            for event in melody
        } if tonic is not None else set()
        required_degrees = (
            {3, 4}
            if profile_id == "blues_dominant"
            else {3, 10}
        )
        if not required_degrees.issubset(relative_pitch_classes):
            names = (
                "minor and major third"
                if profile_id == "blues_dominant"
                else "minor third and flat seventh"
            )
            failures.append(
                f"Blues melody does not state both {names}"
            )
        base_harmony = recipe.get("progression", {}).get(
            "base_harmony", []
        )
        final_roman = (
            str(base_harmony[-1].get("roman", ""))
            .split(" ", 1)[0]
            if base_harmony else ""
        )
        if not final_roman.startswith(("I", "i", "V", "v")):
            failures.append(
                "Blues ending is neither an explicit tonic close nor "
                "an open dominant route"
            )
        if (
            profile_id == "blues_minor" and
            int(recipe.get("complexity", 0)) < 5 and
            any(
                str(event.get("roman", "")).startswith("iiø") or
                str(event.get("roman", "")).startswith("V7")
                for event in base_harmony
            )
        ):
            failures.append(
                "foundation Minor Blues uses the later functional iiø/V7 "
                "turnaround vocabulary"
            )
        bars = max(1, int(recipe.get("bars", 1)))
        beats_per_bar = max(
            1, int(recipe.get("beats_per_bar", 1))
        )
        pulse_units = max(
            1,
            int(
                recipe.get("time", {}).get(
                    "tempo_pulse_units", 1
                )
            ),
        )
        bass = role_events(recipe, "bass")
        if pulse_units > 1:
            if len(bass) / bars > 6.0:
                failures.append(
                    "compound Blues bass exceeds six attacks per bar"
                )
            off_pulse = [
                event for event in bass
                if (
                    int(event.get("tick", 0)) //
                    TICKS_PER_BEAT
                ) % beats_per_bar % pulse_units != 0
            ]
            if (
                int(recipe.get("complexity", 0)) < 5 and
                off_pulse
            ):
                failures.append(
                    "foundation compound Blues bass attacks between "
                    "dotted-quarter pulse anchors"
                )
        earliest_turnaround_beat = max(
            0, bars - 3
        ) * beats_per_bar
        for decision in recipe.get("progression", {}).get(
            "theory_decisions", []
        ):
            if int(decision.get("beat", 0)) < earliest_turnaround_beat:
                failures.append(
                    "Blues chromatic complexity operation occurs before "
                    "the closing turnaround span"
                )
                break
        melody_per_bar = [0 for _ in range(bars)]
        for event in melody:
            bar = min(
                bars - 1,
                max(
                    0,
                    int(event.get("tick", 0)) //
                    (beats_per_bar * TICKS_PER_BEAT),
                ),
            )
            melody_per_bar[bar] += 1
        shaped_sections = 0
        eligible_sections = 0
        for section in sections:
            start = int(section.get("start_bar", 1)) - 1
            length = int(section.get("bars", 0))
            if length < 2:
                continue
            eligible_sections += 1
            split = start + max(1, length // 2)
            end = min(bars, start + length)
            if (
                sum(melody_per_bar[start:split]) >=
                sum(melody_per_bar[split:end])
            ):
                shaped_sections += 1
        if (
            eligible_sections and
            shaped_sections * 2 < eligible_sections
        ):
            failures.append(
                "Blues lead does not preserve call/answer breathing "
                "across most form lines"
            )
        if len(sections) > 1:
            def call_onsets(section: dict[str, Any]) -> set[int]:
                start_bar = int(
                    section.get("start_bar", 1)
                ) - 1
                length = max(
                    1, int(section.get("bars", 1))
                )
                start_tick = (
                    start_bar * beats_per_bar *
                    TICKS_PER_BEAT
                )
                call_end_tick = start_tick + (
                    max(1, length // 2) *
                    beats_per_bar *
                    TICKS_PER_BEAT
                )
                return {
                    int(event.get("tick", 0)) -
                    start_tick
                    for event in melody
                    if start_tick <=
                    int(event.get("tick", 0)) <
                    call_end_tick
                }

            opening_call = call_onsets(sections[0])
            later_calls = [
                call_onsets(section)
                for section in sections[1:]
            ]
            best_recall = max(
                (
                    len(opening_call & later) /
                    max(1, len(opening_call))
                    for later in later_calls
                ),
                default=0.0,
            )
            if opening_call and best_recall < 0.5:
                failures.append(
                    "later Blues lines do not recall at least half of the "
                    "opening call onset template"
                )
        support = role_events(recipe, "support")
        if int(recipe.get("complexity", 0)) >= 6 and not support:
            failures.append(
                "advanced Blues omits the instrumental response role"
            )
        for event in support:
            event_bar = (
                int(event.get("tick", 0)) //
                (beats_per_bar * TICKS_PER_BEAT)
            )
            section = next(
                (
                    row for row in sections
                    if int(row.get("start_bar", 1)) - 1
                    <= event_bar
                    < int(row.get("start_bar", 1)) - 1
                    + int(row.get("bars", 0))
                ),
                None,
            )
            if section is None:
                continue
            local_bar = (
                event_bar -
                (int(section.get("start_bar", 1)) - 1)
            )
            if local_bar < max(
                1, int(section.get("bars", 0)) // 2
            ):
                failures.append(
                    "Blues instrumental response enters during the lead "
                    "call half of a form line"
                )
                break
    return failures


def lane_pair_measurements(
    samples: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    signatures: dict[str, list[tuple[tuple[Any, ...], ...]]] = {
        "harmony": [],
        "melody": [],
        "bass": [],
        "support": [],
        "drums": [],
    }
    for sample in samples:
        recipe = sample["recipe"]
        signatures["harmony"].append(harmony_signature(recipe, normalized=True))
        signatures["melody"].append(
            event_signature(role_events(recipe, "melody"), True, melody=True)
        )
        signatures["bass"].append(
            event_signature(role_events(recipe, "bass"), True)
        )
        signatures["support"].append(
            event_signature(role_events(recipe, "support"), True)
        )
        signatures["drums"].append(drum_signature(recipe, expressive=False))
    result: dict[str, Any] = {}
    for lane, values in signatures.items():
        exact, _ = count_equal_pairs(values)
        similarities = [
            jaccard(signature_set(values[left]), signature_set(values[right]))
            for left, right in pair_indices(len(values))
        ]
        result[lane] = {
            "exact_duplicate_pairs": exact,
            "pair_count": len(similarities),
            "similarity": summarize(similarities),
        }
    return result


def analyze_group(
    key: tuple[str, str, int],
    samples: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    raw = [complete_signature(sample, normalized=False) for sample in samples]
    normalized = [
        complete_signature(sample, normalized=True) for sample in samples
    ]
    raw_count, raw_pairs = count_equal_pairs(raw)
    normalized_count, normalized_pairs = count_equal_pairs(normalized)
    ids = [str(sample.get("id", "")) for sample in samples]
    return {
        "profile_id": key[0],
        "form_id": key[1],
        "complexity": key[2],
        "sample_count": len(samples),
        "pair_count": math.comb(len(samples), 2) if len(samples) >= 2 else 0,
        "exact_raw_duplicate_pairs": raw_count,
        "exact_normalized_duplicate_pairs": normalized_count,
        "raw_duplicate_sample_pairs": [
            [ids[left], ids[right]] for left, right in raw_pairs
        ],
        "normalized_duplicate_sample_pairs": [
            [ids[left], ids[right]] for left, right in normalized_pairs
        ],
        "lanes": lane_pair_measurements(samples),
    }


def selected_complexity_tools(recipe: dict[str, Any]) -> set[str]:
    return {
        str(tool.get("tool_id", ""))
        for tool in recipe.get("complexity_tools", [])
        if bool(tool.get("selected", False))
    }


def analyze_complexity_group(
    key: tuple[str, str, int],
    samples: Sequence[dict[str, Any]],
    matched_seeds: bool,
) -> dict[str, Any]:
    ordered = sorted(
        samples, key=lambda sample: int(sample.get("requested_complexity", 0))
    )
    transitions = []
    previous_tools: set[str] | None = None
    previous_sample: dict[str, Any] | None = None
    lost_tools: set[str] = set()
    for sample in ordered:
        recipe = sample["recipe"]
        tools = selected_complexity_tools(recipe)
        if previous_tools is not None and previous_sample is not None:
            new_tools = tools - previous_tools
            removed_tools = previous_tools - tools
            lost_tools.update(removed_tools)
            previous_recipe = previous_sample["recipe"]
            transitions.append(
                {
                    "from_complexity": int(
                        previous_sample.get("requested_complexity", 0)
                    ),
                    "to_complexity": int(
                        sample.get("requested_complexity", 0)
                    ),
                    "new_selected_tools": sorted(new_tools),
                    "lost_selected_tools": sorted(removed_tools),
                    "event_count_change": {
                        lane: (
                            len(role_events(recipe, lane))
                            - len(role_events(previous_recipe, lane))
                        )
                        for lane in ("melody", "bass", "support")
                    },
                    "harmony_event_count_change": (
                        len(recipe.get("progression", {}).get("base_harmony", []))
                        - len(
                            previous_recipe.get("progression", {}).get(
                                "base_harmony", []
                            )
                        )
                    ),
                    "drums_exactly_preserved": (
                        drum_signature(recipe, expressive=True)
                        == drum_signature(previous_recipe, expressive=True)
                    ),
                    "progression_preserved": (
                        recipe.get("progression", {}).get("id")
                        == previous_recipe.get("progression", {}).get("id")
                    ),
                    "variation_plan_preserved": (
                        recipe.get("variation", {}).get("id")
                        == previous_recipe.get("variation", {}).get("id")
                    ),
                    "tempo_preserved": (
                        int(recipe.get("bpm", 0))
                        == int(previous_recipe.get("bpm", 0))
                    ),
                    "motif_cell_preserved": (
                        recipe.get("motif", {}).get("cell")
                        == previous_recipe.get("motif", {}).get("cell")
                    ),
                }
            )
        previous_tools = tools
        previous_sample = sample
    seeds = {str(sample.get("seed", "")) for sample in ordered}
    return {
        "profile_id": key[0],
        "form_id": key[1],
        "sample_index": key[2],
        "complexities": [
            int(sample.get("requested_complexity", 0)) for sample in ordered
        ],
        "seeds": sorted(seeds),
        "matched_seed_expected": matched_seeds,
        "matched_seed_valid": not matched_seeds or len(seeds) == 1,
        "selected_tools": {
            str(sample.get("requested_complexity", 0)): sorted(
                selected_complexity_tools(sample["recipe"])
            )
            for sample in ordered
        },
        "lost_selected_tools": sorted(lost_tools),
        "transitions": transitions,
    }


def counter_dict(values: Iterable[str]) -> dict[str, int]:
    return dict(sorted(Counter(values).items()))


def gmd_comparison(
    profile_id: str,
    samples: Sequence[dict[str, Any]],
    gmd: dict[str, Any] | None,
) -> dict[str, Any]:
    source_style, relationship, note = GMD_PROFILE_MAP.get(
        profile_id, (None, "unsupported", "No declared GMD mapping")
    )
    result: dict[str, Any] = {
        "relationship": relationship,
        "source_style": source_style or "",
        "note": note,
    }
    if not gmd or not source_style:
        return result
    source_table = (
        "byExactStyle"
        if profile_id in GMD_EXACT_STYLE_PROFILES
        else "byPrimaryStyle"
    )
    source = gmd.get(source_table, {}).get(source_style)
    if not source:
        result["relationship"] = "unsupported"
        result["note"] += "; source population is absent from this evidence file"
        return result
    jam_hits_per_bar = []
    velocities: dict[str, list[int]] = defaultdict(list)
    for sample in samples:
        recipe = sample["recipe"]
        bars = max(1, int(recipe.get("bars", 1)))
        events = recipe.get("groove", {}).get("performance_events", [])
        jam_hits_per_bar.append(len(events) / bars)
        for event in events:
            velocities[str(event.get("lane", ""))].append(
                int(event.get("velocity", 0))
            )
    result["source_files"] = int(source.get("files", 0))
    result["hits_per_bar"] = {
        "jam2": summarize(jam_hits_per_bar),
        "gmd": source.get("hitsPerBar", {"count": 0}),
    }
    velocity_rows: dict[str, Any] = {}
    for jam_lane, gmd_lane in GMD_LANE_NAMES.items():
        gmd_velocity = (
            source.get("pieces", {})
            .get(gmd_lane, {})
            .get("velocity")
        )
        if velocities.get(jam_lane) and gmd_velocity:
            jam_summary = summarize(velocities[jam_lane])
            velocity_rows[jam_lane] = {
                "jam2": jam_summary,
                "gmd": gmd_velocity,
                "median_difference": round(
                    float(jam_summary.get("median", 0))
                    - float(gmd_velocity.get("median", 0)),
                    3,
                ),
            }
    result["velocity_by_lane"] = velocity_rows
    return result


def profile_report(
    profile_id: str,
    samples: Sequence[dict[str, Any]],
    groups: Sequence[dict[str, Any]],
    complexity_groups: Sequence[dict[str, Any]],
    gmd: dict[str, Any] | None,
) -> dict[str, Any]:
    recipes = [sample["recipe"] for sample in samples]
    metrics = [sample_metrics(sample) for sample in samples]
    contract_rows = [
        {"sample_id": sample["id"], "failures": failures}
        for sample in samples
        if (failures := contract_failures(sample))
    ]
    total_pairs = sum(int(group["pair_count"]) for group in groups)
    raw_duplicates = sum(
        int(group["exact_raw_duplicate_pairs"]) for group in groups
    )
    normalized_duplicates = sum(
        int(group["exact_normalized_duplicate_pairs"]) for group in groups
    )
    lane_rows: dict[str, Any] = {}
    for lane in ("harmony", "melody", "bass", "support", "drums"):
        similarities = []
        duplicates = 0
        pair_count = 0
        for group in groups:
            row = group["lanes"][lane]
            duplicates += int(row["exact_duplicate_pairs"])
            pair_count += int(row["pair_count"])
            # Aggregate summaries cannot recover every point, so retain group
            # summaries separately and summarize their medians here.
            if row["similarity"].get("count"):
                similarities.append(float(row["similarity"]["median"]))
        lane_rows[lane] = {
            "exact_duplicate_pairs": duplicates,
            "pair_count": pair_count,
            "duplicate_pair_ratio": ratio(duplicates, pair_count),
            "group_similarity_medians": summarize(similarities),
        }
    progression_ids = counter_dict(
        str(recipe.get("progression", {}).get("id", "")) for recipe in recipes
    )
    progression_families = counter_dict(
        str(recipe.get("progression", {}).get("family_id", ""))
        for recipe in recipes
    )
    groove_ids = counter_dict(
        str(recipe.get("groove", {}).get("id", "")) for recipe in recipes
    )
    motif_cells = counter_dict(
        str(recipe.get("motif", {}).get("cell", "")) for recipe in recipes
    )
    motif_rhythms = counter_dict(
        str(recipe.get("motif", {}).get("rhythm", "")) for recipe in recipes
    )
    arrangement_roles = counter_dict(
        " > ".join(
            str(section.get("role", ""))
            for section in recipe.get("native_form", {}).get("sections", [])
        )
        for recipe in recipes
    )
    events_per_bar = {
        lane: summarize(
            float(metric["events_per_bar"][lane]) for metric in metrics
        )
        for lane in ("harmony", "melody", "bass", "support", "drums")
    }
    drum_flow = {
        field: summarize(
            float(metric["drum_flow"][field]) for metric in metrics
        )
        for field in (
            "unique_backbone_bars",
            "unique_detailed_bars",
            "backbone_reuse_ratio",
            "adjacent_backbone_change_ratio",
            "fill_bar_count",
            "fill_bar_ratio",
            "structural_fill_ratio",
            "unique_backbone_phrases",
        )
    }
    melody_recurrence = {
        "first_to_final_similarity": summarize(
            float(metric["melody_recurrence"]["first_to_final_similarity"])
            for metric in metrics
        ),
        "section_pair_similarity": summarize(
            float(pair["similarity"])
            for metric in metrics
            for pair in metric["melody_recurrence"]["section_pairs"]
        ),
    }
    four_bar_flow = {
        lane: {
            "unique_block_ratio": summarize(
                float(
                    metric["four_bar_flow"]["lanes"][lane][
                        "unique_block_ratio"
                    ]
                )
                for metric in metrics
            ),
            "exact_adjacent_repeat_ratio": summarize(
                float(
                    metric["four_bar_flow"]["lanes"][lane][
                        "exact_adjacent_repeat_ratio"
                    ]
                )
                for metric in metrics
            ),
            "adjacent_similarity": summarize(
                float(
                    metric["four_bar_flow"]["lanes"][lane][
                        "adjacent_similarity"
                    ].get("median", 0.0)
                )
                for metric in metrics
            ),
            "first_to_final_similarity": summarize(
                float(
                    metric["four_bar_flow"]["lanes"][lane][
                        "first_to_final_similarity"
                    ]
                )
                for metric in metrics
            ),
        }
        for lane in ("harmony", "melody", "bass", "support", "drums")
    }
    full_forms = [metric["full_form"] for metric in metrics]
    boundary_rows = [
        boundary
        for form in full_forms
        for boundary in form["boundaries"]
    ]
    endings = [form["ending"] for form in full_forms]
    full_form = {
        "section_coverage_failure_count": sum(
            not bool(form["section_coverage_valid"])
            for form in full_forms
        ),
        "drum_phrase_plan_coverage_failure_count": sum(
            not bool(form["drum_phrase_plan_coverage_valid"])
            for form in full_forms
        ),
        "boundary_count": len(boundary_rows),
        "boundary_with_lead_in_fill_ratio": ratio(
            sum(int(row["lead_in_fill_hits"]) > 0 for row in boundary_rows),
            len(boundary_rows),
        ),
        "boundary_with_landing_accent_ratio": ratio(
            sum(int(row["landing_accent_hits"]) > 0 for row in boundary_rows),
            len(boundary_rows),
        ),
        "boundary_with_bass_landing_ratio": ratio(
            sum(bool(row["bass_lands_at_boundary"]) for row in boundary_rows),
            len(boundary_rows),
        ),
        "boundary_harmonic_change_ratio": ratio(
            sum(bool(row["harmony_changes"]) for row in boundary_rows),
            len(boundary_rows),
        ),
        "melody_boundary_absolute_interval": summarize(
            abs(int(row["melody_boundary_interval"]))
            for row in boundary_rows
            if row["melody_boundary_interval"] is not None
        ),
        "melody_boundary_gap_beats": summarize(
            float(row["melody_gap_ticks"]) / TICKS_PER_BEAT
            for row in boundary_rows
            if int(row["melody_gap_ticks"]) >= 0
        ),
        "ending_final_chord_root_is_tonic_ratio": ratio(
            sum(bool(ending["final_chord_root_is_tonic"]) for ending in endings),
            len(endings),
        ),
        "ending_final_melody_is_chord_tone_ratio": ratio(
            sum(bool(ending["final_melody_is_chord_tone"]) for ending in endings),
            len(endings),
        ),
        "ending_final_melody_is_tonic_ratio": ratio(
            sum(bool(ending["final_melody_is_tonic"]) for ending in endings),
            len(endings),
        ),
        "ending_last_bar_fill_hits": summarize(
            int(ending["last_bar_fill_hits"]) for ending in endings
        ),
        "drum_energy_trajectories": counter_dict(
            ",".join(str(value) for value in form["drum_energy_trajectory"])
            for form in full_forms
        ),
    }
    theory_rows = [
        row
        for metric in metrics
        for row in metric["theory_operations"]
    ]
    theory_failure_rows = [
        row for row in theory_rows if row["failures"]
    ]
    findings = []
    if raw_duplicates:
        findings.append(
            {
                "kind": "exact-complete-duplicate",
                "count": raw_duplicates,
                "detail": "Complete raw arrangements repeat within a fixed form/complexity seed group.",
            }
        )
    if normalized_duplicates:
        findings.append(
            {
                "kind": "normalized-complete-duplicate",
                "count": normalized_duplicates,
                "detail": "Complete arrangements repeat after removing absolute transposition.",
            }
        )
    if contract_rows:
        findings.append(
            {
                "kind": "research-contract",
                "count": len(contract_rows),
                "detail": "Generated samples contradict embedded profile/form constraints.",
            }
        )
    if theory_failure_rows:
        findings.append(
            {
                "kind": "theory-operation-contract",
                "count": len(theory_failure_rows),
                "detail": (
                    "An applied complexity operation failed its functional "
                    "resolution or coordinated-lane contract."
                ),
            }
        )
    invalid_complexity_seeds = sum(
        not bool(group["matched_seed_valid"]) for group in complexity_groups
    )
    lost_complexity_tools = sum(
        len(group["lost_selected_tools"]) for group in complexity_groups
    )
    if invalid_complexity_seeds:
        findings.append(
            {
                "kind": "complexity-seed-mismatch",
                "count": invalid_complexity_seeds,
                "detail": "A matched complexity comparison did not retain its seed.",
            }
        )
    transition_rows = [
        transition
        for group in complexity_groups
        for transition in group["transitions"]
    ]
    selected_by_complexity: dict[str, Counter[str]] = defaultdict(Counter)
    for group in complexity_groups:
        for complexity, tools in group["selected_tools"].items():
            selected_by_complexity[complexity].update(tools)
    return {
        "style_id": str(samples[0].get("style_id", "")),
        "profile_id": profile_id,
        "profile_name": str(samples[0].get("profile_name", profile_id)),
        "sample_count": len(samples),
        "forms": counter_dict(str(sample.get("form_id", "")) for sample in samples),
        "complexities": counter_dict(
            str(sample.get("requested_complexity", "")) for sample in samples
        ),
        "group_count": len(groups),
        "pair_count": total_pairs,
        "exact_raw_duplicate_pairs": raw_duplicates,
        "exact_normalized_duplicate_pairs": normalized_duplicates,
        "exact_raw_duplicate_pair_ratio": ratio(raw_duplicates, total_pairs),
        "exact_normalized_duplicate_pair_ratio": ratio(
            normalized_duplicates, total_pairs
        ),
        "lane_pair_measurements": lane_rows,
        "variation_coverage": {
            "progression_ids": progression_ids,
            "progression_families": progression_families,
            "normalized_harmony_signatures": len(
                {harmony_signature(recipe, True) for recipe in recipes}
            ),
            "groove_ids": groove_ids,
            "normalized_drum_signatures": len(
                {drum_signature(recipe, False) for recipe in recipes}
            ),
            "backbone_bar_library_size": len({
                signature
                for recipe in recipes
                for signature in drum_backbone_bar_signatures(recipe)
            }),
            "backbone_sequence_library_size": len({
                drum_backbone_bar_signatures(recipe)
                for recipe in recipes
            }),
            "motif_cells": motif_cells,
            "motif_rhythms": motif_rhythms,
            "normalized_melody_signatures": len(
                {
                    event_signature(
                        role_events(recipe, "melody"), True, melody=True
                    )
                    for recipe in recipes
                }
            ),
            "normalized_bass_signatures": len(
                {
                    event_signature(role_events(recipe, "bass"), True)
                    for recipe in recipes
                }
            ),
            "normalized_support_signatures": len(
                {
                    event_signature(role_events(recipe, "support"), True)
                    for recipe in recipes
                }
            ),
            "modes": counter_dict(str(recipe.get("mode", "")) for recipe in recipes),
            "tonics": counter_dict(
                str(recipe.get("tonic", "")) for recipe in recipes
            ),
            "variation_ids": counter_dict(
                str(recipe.get("variation", {}).get("id", ""))
                for recipe in recipes
            ),
            "arrangement_role_sequences": arrangement_roles,
        },
        "structure": {
            "bpm": summarize(int(metric["bpm"]) for metric in metrics),
            "events_per_bar": events_per_bar,
            "melody_pitch_range": summarize(
                int(metric["melody_pitch_range"]) for metric in metrics
            ),
            "section_count": summarize(
                int(metric["section_count"]) for metric in metrics
            ),
            "section_density_change_count": summarize(
                int(metric["section_density_change_count"]) for metric in metrics
            ),
            "kick_bass_onset_overlap": summarize(
                float(metric["kick_bass_onset_overlap"]) for metric in metrics
            ),
            "melody_support_unison_collision": summarize(
                float(metric["melody_support_unison_collision"])
                for metric in metrics
            ),
            "drum_flow": drum_flow,
            "melody_recurrence": melody_recurrence,
            "four_bar_flow": four_bar_flow,
            "full_form": full_form,
        },
        "theory_operation_audit": {
            "operation_count": len(theory_rows),
            "kinds": counter_dict(
                str(row["kind"]) for row in theory_rows
            ),
            "target_at_phrase_start_ratio": ratio(
                sum(bool(row["target_at_phrase_start"]) for row in theory_rows),
                len(theory_rows),
            ),
            "target_at_section_start_ratio": ratio(
                sum(bool(row["target_at_section_start"]) for row in theory_rows),
                len(theory_rows),
            ),
            "failure_count": len(theory_failure_rows),
            "failures": theory_failure_rows,
        },
        "complexity_development": {
            "matched_groups": len(complexity_groups),
            "invalid_seed_groups": invalid_complexity_seeds,
            "lost_selected_tool_count": lost_complexity_tools,
            "selected_tool_turnover_note": (
                "Optional selected-tool turnover is descriptive. The unlocked "
                "palette must be cumulative, but a higher-complexity "
                "realisation need not force every optional device selected by "
                "a lower-complexity realisation."
            ),
            "selected_tool_counts": {
                complexity: dict(sorted(counts.items()))
                for complexity, counts in sorted(selected_by_complexity.items())
            },
            "transition_count": len(transition_rows),
            "drum_preservation": {
                "preserved": sum(
                    bool(row["drums_exactly_preserved"])
                    for row in transition_rows
                ),
                "changed": sum(
                    not bool(row["drums_exactly_preserved"])
                    for row in transition_rows
                ),
            },
            "progression_preservation": {
                "preserved": sum(
                    bool(row["progression_preserved"]) for row in transition_rows
                ),
                "changed": sum(
                    not bool(row["progression_preserved"])
                    for row in transition_rows
                ),
            },
            "variation_plan_preservation": {
                "preserved": sum(
                    bool(row["variation_plan_preserved"])
                    for row in transition_rows
                ),
                "changed": sum(
                    not bool(row["variation_plan_preserved"])
                    for row in transition_rows
                ),
            },
            "tempo_preservation": {
                "preserved": sum(
                    bool(row["tempo_preserved"]) for row in transition_rows
                ),
                "changed": sum(
                    not bool(row["tempo_preserved"]) for row in transition_rows
                ),
            },
            "motif_cell_preservation": {
                "preserved": sum(
                    bool(row["motif_cell_preserved"])
                    for row in transition_rows
                ),
                "changed": sum(
                    not bool(row["motif_cell_preserved"])
                    for row in transition_rows
                ),
            },
        },
        "contract_failures": contract_rows,
        "gmd_comparison": gmd_comparison(profile_id, samples, gmd),
        "findings": findings,
    }


def analyze(
    corpus: dict[str, Any],
    gmd: dict[str, Any] | None = None,
) -> dict[str, Any]:
    samples = list(corpus.get("samples", []))
    groups_by_key: dict[tuple[str, str, int], list[dict[str, Any]]] = defaultdict(list)
    profiles: dict[str, list[dict[str, Any]]] = defaultdict(list)
    complexity_by_key: dict[
        tuple[str, str, int], list[dict[str, Any]]
    ] = defaultdict(list)
    for sample in samples:
        key = (
            str(sample.get("profile_id", "")),
            str(sample.get("form_id", "")),
            int(sample.get("requested_complexity", 0)),
        )
        groups_by_key[key].append(sample)
        profiles[key[0]].append(sample)
        complexity_by_key[
            (
                key[0],
                key[1],
                int(sample.get("sample_index", 0)),
            )
        ].append(sample)
    group_reports = [
        analyze_group(key, group_samples)
        for key, group_samples in sorted(groups_by_key.items())
    ]
    groups_for_profile: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for group in group_reports:
        groups_for_profile[str(group["profile_id"])].append(group)
    matched_seeds = bool(corpus.get("matched_complexity_seeds", False))
    complexity_group_reports = [
        analyze_complexity_group(key, group_samples, matched_seeds)
        for key, group_samples in sorted(complexity_by_key.items())
    ]
    complexity_for_profile: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for group in complexity_group_reports:
        complexity_for_profile[str(group["profile_id"])].append(group)
    profile_reports = {
        profile_id: profile_report(
            profile_id,
            profile_samples,
            groups_for_profile[profile_id],
            complexity_for_profile[profile_id],
            gmd,
        )
        for profile_id, profile_samples in sorted(profiles.items())
    }
    findings = [
        {
            "profile_id": profile_id,
            **finding,
        }
        for profile_id, report in profile_reports.items()
        for finding in report["findings"]
    ]
    return {
        "schema": SCHEMA,
        "source": {
            "generator": corpus.get("generator", ""),
            "generated_at": corpus.get("generated_at", ""),
            "seed_namespace": corpus.get("seed_namespace", ""),
            "sample_count": len(samples),
            "profile_count": len(profile_reports),
            "group_count": len(group_reports),
            "complexity_group_count": len(complexity_group_reports),
            "matched_complexity_seeds": matched_seeds,
            "gmd_schema": gmd.get("schema", "") if gmd else "",
        },
        "method": {
            "comparison_cell": "fixed profile, native form, and complexity",
            "transposition_normalization": (
                "Roman harmony plus interval-relative melody, bass, and support; "
                "drum placement excludes expression but retains role/fill state"
            ),
            "similarity": "Jaccard similarity over lane event signatures",
            "style_quality_policy": (
                "Raw evidence only; similarity and structural-variant counts "
                "are not converted into subjective quality scores"
            ),
        },
        "profiles": profile_reports,
        "groups": group_reports,
        "complexity_groups": complexity_group_reports,
        "findings": findings,
    }


def format_number(value: Any, digits: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def markdown_report(audit: dict[str, Any]) -> str:
    source = audit["source"]
    lines = [
        "# Jam2 Symbolic Generation Baseline",
        "",
        f"- Samples: `{source['sample_count']}`",
        f"- Profiles: `{source['profile_count']}`",
        f"- Fixed comparison cells: `{source['group_count']}`",
        f"- Seed namespace: `{source['seed_namespace']}`",
        "- Timbre and mix are excluded.",
        "- GMD proxy rows are descriptive and are not direct style validation.",
        "",
        "## Profile overview",
        "",
        "| Style | Profile | Samples | Raw full duplicates | Normalized full duplicates | Harmony sigs | Melody sigs | Bass sigs | Drum sigs | GMD relation |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    profiles = sorted(
        audit["profiles"].values(),
        key=lambda row: (row["style_id"], row["profile_name"]),
    )
    for profile in profiles:
        coverage = profile["variation_coverage"]
        lines.append(
            "| {style} | {name} | {samples} | {raw}/{pairs} | "
            "{normalized}/{pairs} | {harmony} | {melody} | {bass} | "
            "{drums} | {gmd} |".format(
                style=profile["style_id"],
                name=profile["profile_name"],
                samples=profile["sample_count"],
                raw=profile["exact_raw_duplicate_pairs"],
                normalized=profile["exact_normalized_duplicate_pairs"],
                pairs=profile["pair_count"],
                harmony=coverage["normalized_harmony_signatures"],
                melody=coverage["normalized_melody_signatures"],
                bass=coverage["normalized_bass_signatures"],
                drums=coverage["normalized_drum_signatures"],
                gmd=profile["gmd_comparison"]["relationship"],
            )
        )
    lines.extend(["", "## Style-by-style evidence", ""])
    current_style = None
    for profile in profiles:
        if profile["style_id"] != current_style:
            current_style = profile["style_id"]
            lines.extend([f"### {current_style}", ""])
        coverage = profile["variation_coverage"]
        structure = profile["structure"]
        complexity = profile["complexity_development"]
        theory = profile["theory_operation_audit"]
        gmd = profile["gmd_comparison"]
        lines.extend(
            [
                f"#### {profile['profile_name']} (`{profile['profile_id']}`)",
                "",
                (
                    f"- Comparison: `{profile['group_count']}` cells, "
                    f"`{profile['pair_count']}` within-cell pairs; "
                    f"raw duplicates `{profile['exact_raw_duplicate_pairs']}`, "
                    f"normalized duplicates `{profile['exact_normalized_duplicate_pairs']}`."
                ),
                (
                    "- Distinct vocabulary across the corpus: "
                    f"`{len(coverage['progression_ids'])}` progression IDs, "
                    f"`{len(coverage['groove_ids'])}` grooves, "
                    f"`{coverage['backbone_bar_library_size']}` core drum bars, "
                    f"`{coverage['backbone_sequence_library_size']}` core drum sequences, "
                    f"`{len(coverage['motif_cells'])}` motif cells, "
                    f"`{coverage['normalized_melody_signatures']}` normalized melodies, "
                    f"`{coverage['normalized_bass_signatures']}` normalized bass lines, "
                    f"`{coverage['normalized_support_signatures']}` normalized support lines."
                ),
                (
                    "- Median events/bar: harmony "
                    f"`{format_number(structure['events_per_bar']['harmony'].get('median'))}`, "
                    f"melody `{format_number(structure['events_per_bar']['melody'].get('median'))}`, "
                    f"bass `{format_number(structure['events_per_bar']['bass'].get('median'))}`, "
                    f"support `{format_number(structure['events_per_bar']['support'].get('median'))}`, "
                    f"drums `{format_number(structure['events_per_bar']['drums'].get('median'))}`."
                ),
                (
                    "- Median section-density boundary changes: "
                    f"`{format_number(structure['section_density_change_count'].get('median'))}`; "
                    "kick/bass onset overlap "
                    f"`{format_number(structure['kick_bass_onset_overlap'].get('median'))}`; "
                    "melody/support exact unison collision "
                    f"`{format_number(structure['melody_support_unison_collision'].get('median'))}`."
                ),
                (
                    "- Drum flow medians: "
                    f"`{format_number(structure['drum_flow']['unique_backbone_bars'].get('median'))}` "
                    "non-ghost structural bar variants, "
                    f"`{format_number(structure['drum_flow']['unique_detailed_bars'].get('median'))}` "
                    "performed bar variants, backbone reuse "
                    f"`{format_number(structure['drum_flow']['backbone_reuse_ratio'].get('median'))}`, "
                    "structurally placed fills "
                    f"`{format_number(structure['drum_flow']['structural_fill_ratio'].get('median'))}`."
                ),
                (
                    "- Melodic form medians: first-to-final section recurrence "
                    f"`{format_number(structure['melody_recurrence']['first_to_final_similarity'].get('median'))}`; "
                    "all section-pair recurrence "
                    f"`{format_number(structure['melody_recurrence']['section_pair_similarity'].get('median'))}`."
                ),
                (
                    "- Four-bar flow medians (exact adjacent repeat / adjacent similarity): "
                    "harmony "
                    f"`{format_number(structure['four_bar_flow']['harmony']['exact_adjacent_repeat_ratio'].get('median'))}`/"
                    f"`{format_number(structure['four_bar_flow']['harmony']['adjacent_similarity'].get('median'))}`, "
                    "melody "
                    f"`{format_number(structure['four_bar_flow']['melody']['exact_adjacent_repeat_ratio'].get('median'))}`/"
                    f"`{format_number(structure['four_bar_flow']['melody']['adjacent_similarity'].get('median'))}`, "
                    "bass "
                    f"`{format_number(structure['four_bar_flow']['bass']['exact_adjacent_repeat_ratio'].get('median'))}`/"
                    f"`{format_number(structure['four_bar_flow']['bass']['adjacent_similarity'].get('median'))}`, "
                    "drums "
                    f"`{format_number(structure['four_bar_flow']['drums']['exact_adjacent_repeat_ratio'].get('median'))}`/"
                    f"`{format_number(structure['four_bar_flow']['drums']['adjacent_similarity'].get('median'))}`."
                ),
                (
                    "- Applied complexity operations: "
                    f"`{theory['operation_count']}`; functional/lane failures "
                    f"`{theory['failure_count']}`; destinations at phrase starts "
                    f"`{format_number(theory['target_at_phrase_start_ratio'])}` "
                    "and section starts "
                    f"`{format_number(theory['target_at_section_start_ratio'])}`."
                ),
                (
                    "- Matched complexity development: "
                    f"`{complexity['matched_groups']}` seed groups; "
                    f"optional selected-tool turnovers `{complexity['lost_selected_tool_count']}`; "
                    f"drum changes `{complexity['drum_preservation']['changed']}/"
                    f"{complexity['transition_count']}`; progression changes "
                    f"`{complexity['progression_preservation']['changed']}/"
                    f"{complexity['transition_count']}`; tempo changes "
                    f"`{complexity['tempo_preservation']['changed']}/"
                    f"{complexity['transition_count']}`; motif-cell changes "
                    f"`{complexity['motif_cell_preservation']['changed']}/"
                    f"{complexity['transition_count']}`."
                ),
                f"- GMD evidence: **{gmd['relationship']}** — {gmd['note']}.",
            ]
        )
        if gmd.get("hits_per_bar"):
            jam = gmd["hits_per_bar"]["jam2"]
            source_hits = gmd["hits_per_bar"]["gmd"]
            lines.append(
                "- Drum density medians: "
                f"Jam2 `{format_number(jam.get('median'))}` hits/bar; "
                f"GMD `{format_number(source_hits.get('median'))}` hits/bar "
                f"from `{gmd.get('source_files', 0)}` files."
            )
        if profile["contract_failures"]:
            lines.append(
                f"- Research-contract failures: `{len(profile['contract_failures'])}`."
            )
        if profile["findings"]:
            lines.append(
                "- Findings: "
                + "; ".join(
                    f"{finding['kind']} ({finding['count']})"
                    for finding in profile["findings"]
                )
                + "."
            )
        else:
            lines.append("- Automated exact/contract findings: none.")
        lines.append("")
    lines.extend(
        [
            "## Interpretation boundary",
            "",
            "This report establishes reproducible outliers and comparison material. "
            "It does not by itself prove that a progression, melody, bass line, fill, "
            "or arrangement is stylistically convincing. Each profile still requires "
            "a musical review against its authored research brief before acceptance.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--gmd-evidence", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with args.corpus.open("r", encoding="utf-8") as source:
        corpus = json.load(source)
    gmd = None
    if args.gmd_evidence:
        with args.gmd_evidence.open("r", encoding="utf-8") as source:
            gmd = json.load(source)
    audit = analyze(corpus, gmd)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(audit, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown_report(audit), encoding="utf-8")
    print(
        f"Wrote {audit['source']['sample_count']} samples, "
        f"{audit['source']['profile_count']} profiles, and "
        f"{len(audit['findings'])} exact/contract findings to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
