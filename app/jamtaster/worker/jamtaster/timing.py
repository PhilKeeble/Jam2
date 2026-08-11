from __future__ import annotations

from bisect import bisect_right
from collections import Counter
import math
import statistics

from .types import Analysis, DrumHit, NoteEvent, SectionChoice, TimedLabel


ROLE_ORDER = ("bookend", "verse", "chorus", "bridge")
ROLE_NAMES = {
    "bookend": "Bookend (Intro / Outro)",
    "verse": "Verse / Pre-Chorus",
    "chorus": "Chorus",
    "bridge": "Bridge / Instrumental",
}


def estimate_bpm(beats: list[float]) -> float:
    intervals = [b - a for a, b in zip(beats, beats[1:]) if 0.2 <= b - a <= 2.0]
    if not intervals:
        raise ValueError("beat tracker did not return enough regular beats")
    return 60.0 / statistics.median(intervals)


def infer_meter(beats: list[float], downbeats: list[float], fallback: int = 4) -> int:
    if len(beats) < 3 or len(downbeats) < 2:
        return fallback
    indices = [min(range(len(beats)), key=lambda i: abs(beats[i] - downbeat)) for downbeat in downbeats]
    spacings = [b - a for a, b in zip(indices, indices[1:]) if 2 <= b - a <= 12]
    if not spacings:
        return fallback
    return Counter(spacings).most_common(1)[0][0]


def beat_index_at(beats: list[float], time: float) -> int:
    return min(range(len(beats)), key=lambda index: abs(beats[index] - time))


def _boundary_indices(beats: list[float], downbeats: list[float]) -> list[int]:
    result = sorted({beat_index_at(beats, value) for value in downbeats})
    return result or list(range(len(beats)))


def _snap_segment(
    segment: TimedLabel,
    beats: list[float],
    downbeats: list[float],
    duration: float,
    beats_per_bar: int,
) -> SectionChoice:
    boundaries = _boundary_indices(beats, downbeats)
    first = min(boundaries, key=lambda index: abs(beats[index] - segment.start))
    valid_ends = [
        index for index in boundaries
        if beats_per_bar <= index - first <= 512
        and (index - first) % beats_per_bar == 0
    ]
    if not valid_ends:
        raise ValueError(
            f"could not bound {segment.label!r} with complete tracked bars"
        )
    # Prefer the nearest clear downbeat, with the later boundary winning an
    # exact tie so a structure label does not clip the final musical bar.
    last = min(
        valid_ends,
        key=lambda index: (abs(beats[index] - segment.end), beats[index] < segment.end),
    )
    count = last - first
    end = beats[last]
    return SectionChoice(
        role=(segment.label.strip().lower().replace(" ", "-") or "section"),
        source_label=segment.label,
        start=max(0.0, beats[first]),
        end=min(duration, end),
        first_beat=first,
        beats=count,
    )


def choose_sections(analysis: Analysis, duration: float, mode: str) -> list[SectionChoice]:
    if len(analysis.beats) < 2:
        raise ValueError("at least two detected beats are required")
    if mode == "single" or not analysis.structures:
        beats = max(1, round(duration * analysis.bpm / 60.0))
        beats = max(analysis.beats_per_bar, int(round(beats / analysis.beats_per_bar)) * analysis.beats_per_bar)
        if beats > 512:
            raise ValueError(
                "full-sample fallback exceeds Jam2's 512-beat section limit; "
                "structure detection is required for this WAV"
            )
        return [SectionChoice("full", "Full Sample", 0.0, duration, 0, beats)]

    segments = sorted(
        (item for item in analysis.structures if item.end > item.start),
        key=lambda item: (item.start, item.end),
    )
    if len(segments) > 12:
        raise ValueError(
            f"structure analysis returned {len(segments)} sections; Jam2 supports at most 12"
        )
    selected: list[SectionChoice] = []
    for segment in segments:
        selected.append(_snap_segment(
            segment,
            analysis.beats,
            analysis.downbeats,
            duration,
            analysis.beats_per_bar,
        ))
    return selected


def arrangement_steps(structures: list[TimedLabel], choices: list[SectionChoice]) -> list[dict[str, int]]:
    del structures
    return [
        {"bank": index, "repeats": 1}
        for index in range(min(64, len(choices)))
    ]


def _nearest_grid_position(relative_beat: float, allowed: tuple[int, ...]) -> tuple[int, int, float]:
    best: tuple[int, int, float] | None = None
    for division in allowed:
        step = round(relative_beat * division)
        residual = abs(relative_beat - step / division)
        candidate = (division, step, residual)
        if best is None or residual < best[2] - 1e-9 or (
            abs(residual - best[2]) < 1e-9 and division < best[0]
        ):
            best = candidate
    assert best is not None
    return best


def drum_state(velocity: int) -> str:
    if velocity <= 40:
        return "g"
    if velocity >= 101:
        return "a"
    return "x"


def repair_drum_hits(
    detected: list[DrumHit],
    candidates: list[DrumHit],
    beats: list[float],
    bpm: float,
    beats_per_bar: int,
    division: int,
    thresholds: dict[str, float],
    minimum_repeats: int,
    neighborhood_bars: int,
    enabled: bool,
    rim_mode: bool = False,
) -> tuple[list[DrumHit], list[dict[str, object]]]:
    if not beats:
        return detected, []
    beat_duration = 60.0 / bpm
    anchor = beats[0]
    cells_per_bar = beats_per_bar * division

    def cell(hit: DrumHit) -> int:
        return round((hit.time - anchor) / beat_duration * division)

    diagnostics: list[dict[str, object]] = []
    if rim_mode:
        kick_positions: dict[int, set[int]] = {}
        for hit in detected:
            if hit.lane != "Kick":
                continue
            position = cell(hit)
            kick_positions.setdefault(position % cells_per_bar, set()).add(
                position // cells_per_bar
            )
        peak_support = max((len(bars) for bars in kick_positions.values()), default=0)
        stable_kick_positions = {
            position for position, bars in kick_positions.items()
            if len(bars) >= max(3, math.ceil(peak_support * 0.90))
        }
        retained_kicks = [
            hit for hit in detected
            if hit.lane == "Kick" and cell(hit) % cells_per_bar in stable_kick_positions
        ]
        rim_hits = [hit for hit in detected if hit.lane == "Cross-stick / Rim"]
        normalized: list[DrumHit] = []
        for hit in detected:
            reason = ""
            if hit.lane == "Kick" and hit in retained_kicks:
                normalized.append(DrumHit(
                    hit.time, hit.lane, 122, hit.confidence, hit.energy_ratio,
                    hit.provenance,
                ))
                diagnostics.append({
                    "time": hit.time,
                    "lane": hit.lane,
                    "accepted": True,
                    "reason": "stable_rim_mode_kick_accent",
                    "rim_mode": True,
                })
                continue
            if hit.lane == "Kick":
                reason = "unstable_kick_position"
            elif hit.lane == "Cross-stick / Rim" and any(
                abs(hit.time - kick.time) <= 0.06 for kick in retained_kicks
            ):
                reason = "rim_duplicate_of_kick"
            elif hit.lane in {"Closed HH", "Crash"} and any(
                abs(hit.time - other.time) <= 0.06
                for other in [*retained_kicks, *rim_hits]
            ):
                reason = "cymbal_duplicate_in_rim_mode"
            elif hit.lane == "Snare" and any(
                abs(hit.time - rim.time) <= 0.06 for rim in rim_hits
            ):
                reason = "snare_duplicate_of_rim"
            if reason:
                diagnostics.append({
                    "time": hit.time,
                    "lane": hit.lane,
                    "accepted": False,
                    "reason": reason,
                    "rim_mode": True,
                })
            else:
                normalized.append(hit)
        diagnostics.append({
            "rim_mode": True,
            "reason": "rim_mode_summary",
            "stable_kick_positions": sorted(stable_kick_positions),
            "peak_kick_bar_support": peak_support,
            "input_hits": len(detected),
            "output_hits": len(normalized),
        })
        return sorted(normalized, key=lambda hit: (hit.time, hit.lane)), diagnostics

    if not enabled or not candidates:
        return detected, diagnostics

    occupied: dict[tuple[str, int], DrumHit] = {}
    for hit in detected:
        key = (hit.lane, cell(hit))
        current = occupied.get(key)
        if current is None or (hit.confidence or 0.0) > (current.confidence or 0.0):
            occupied[key] = hit
    candidate_cells: dict[tuple[str, int], DrumHit] = {}
    for hit in candidates:
        position = cell(hit)
        if position < 0 or (hit.lane, position) in occupied:
            continue
        key = (hit.lane, position)
        current = candidate_cells.get(key)
        if current is None or (hit.confidence or 0.0) > (current.confidence or 0.0):
            candidate_cells[key] = hit

    repaired: list[DrumHit] = []
    all_evidence = [*detected, *candidate_cells.values()]
    fill_lanes = {"Snare", "Mid Tom", "Crash"}
    repetition_lanes = {"Snare", "Mid Tom"}
    for (lane, position), hit in sorted(candidate_cells.items(), key=lambda item: item[0][1]):
        bar = position // cells_per_bar
        position_in_bar = position % cells_per_bar
        repeat_support = 0
        for (other_lane, other_position), _other in occupied.items():
            if other_lane != lane or other_position % cells_per_bar != position_in_bar:
                continue
            other_bar = other_position // cells_per_bar
            if 0 < abs(other_bar - bar) <= neighborhood_bars:
                repeat_support += 1
        nearby_fill_evidence = sum(
            1 for other in all_evidence
            if other.lane in fill_lanes and abs(other.time - hit.time) <= beat_duration
        )
        threshold = thresholds.get(lane, 1.0)
        confidence_ratio = (hit.confidence or 0.0) / max(1e-9, threshold)
        reason = ""
        if hit.provenance == "candidate_transient":
            reason = "repaired_transient"
        elif (
            lane in repetition_lanes
            and repeat_support >= minimum_repeats
        ):
            reason = "repaired_repetition"
        elif (
            lane == "Snare"
            and nearby_fill_evidence >= 2
            and confidence_ratio >= 0.85
            and drum_state(hit.velocity) != "g"
        ):
            reason = "repaired_fill_cluster"
        accepted = bool(reason)
        if accepted:
            restored = DrumHit(
                hit.time, hit.lane, hit.velocity, hit.confidence,
                hit.energy_ratio, reason,
            )
            repaired.append(restored)
            occupied[(lane, position)] = restored
        diagnostics.append({
            "time": hit.time,
            "lane": lane,
            "cell": position,
            "bar": bar,
            "position_in_bar": position_in_bar,
            "velocity": hit.velocity,
            "state": drum_state(hit.velocity),
            "confidence": hit.confidence,
            "confidence_ratio": confidence_ratio,
            "energy_ratio": hit.energy_ratio,
            "repeat_support": repeat_support,
            "nearby_fill_evidence": nearby_fill_evidence,
            "accepted": accepted,
            "reason": reason or "insufficient_evidence",
        })
    return sorted([*detected, *repaired], key=lambda hit: (hit.time, hit.lane)), diagnostics


def shape_drum_dynamics(
    hits: list[DrumHit], beats: list[float], bpm: float, beats_per_bar: int,
    division: int, downbeats: list[float] | None = None,
) -> tuple[list[DrumHit], list[dict[str, object]]]:
    """Apply inspectable rhythmic context where audio energy is ambiguous."""
    if not beats or beats_per_bar != 4:
        return hits, []
    beat_duration = 60.0 / bpm
    anchor = beats[0]
    anchor_beat_index = 0
    if downbeats:
        anchor = downbeats[0]
        anchor_beat_index = min(
            range(len(beats)), key=lambda index: abs(beats[index] - anchor)
        )
    cells_per_bar = beats_per_bar * division

    def rhythmic_cell(timestamp: float) -> int:
        if len(beats) < 2 or timestamp < beats[0]:
            return round((timestamp - anchor) / beat_duration * division)
        beat_index = max(0, min(len(beats) - 2, bisect_right(beats, timestamp) - 1))
        local_duration = max(1e-6, beats[beat_index + 1] - beats[beat_index])
        step = round((timestamp - beats[beat_index]) / local_duration * division)
        return (beat_index - anchor_beat_index) * division + step
    shaped: list[DrumHit] = []
    diagnostics: list[dict[str, object]] = []
    verified_tom_times = [
        hit.time for hit in hits
        if hit.lane == "Mid Tom" and hit.provenance == "repaired_transient"
    ]
    for hit in hits:
        if hit.lane == "Closed HH" and any(
            abs(hit.time - tom_time) <= 0.06 for tom_time in verified_tom_times
        ):
            diagnostics.append({
                "time": hit.time,
                "input_state": drum_state(hit.velocity),
                "output_state": "suppressed",
                "rule": "verified_tom_over_hihat_crosstalk",
            })
            continue
        if hit.lane != "Snare":
            shaped.append(hit)
            continue
        cell = rhythmic_cell(hit.time)
        position = cell % cells_per_bar
        velocity = 122 if position in {division, 3 * division} else 38
        shaped.append(DrumHit(
            hit.time, hit.lane, velocity, hit.confidence, hit.energy_ratio,
            hit.provenance,
        ))
        diagnostics.append({
            "time": hit.time,
            "cell": cell,
            "position_in_bar": position,
            "input_state": drum_state(hit.velocity),
            "output_state": drum_state(velocity),
            "rule": "four_four_backbeat_accent_else_ghost",
        })
    return shaped, diagnostics


def quantize_drums(
    hits: list[DrumHit], section: SectionChoice, beats: list[float], bpm: float,
    division: int,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    beat_duration = 60.0 / bpm
    assigned: dict[tuple[int, str, int], DrumHit] = {}
    diagnostics: list[dict[str, object]] = []
    for hit in hits:
        if not section.start <= hit.time < section.end:
            continue
        absolute = (hit.time - section.start) / beat_duration
        global_step = round(absolute * division)
        beat = global_step // division
        step = global_step % division
        if 0 <= beat < section.beats:
            key = (beat, hit.lane, step)
            current = assigned.get(key)
            priority = (hit.provenance == "detected", hit.confidence or 0.0, hit.energy_ratio or 0.0)
            current_priority = (
                current.provenance == "detected", current.confidence or 0.0,
                current.energy_ratio or 0.0,
            ) if current else None
            if current is None or priority > current_priority:
                assigned[key] = hit
    patterns: list[dict[str, object]] = []
    lanes = ("Kick", "Snare", "Closed HH", "Open HH", "Ride", "Crash", "High Tom", "Mid Tom", "Floor Tom", "Cross-stick / Rim")
    for beat in range(section.beats):
        texts = {lane: ["."] * division for lane in lanes}
        for (event_beat, lane, step), hit in assigned.items():
            if event_beat != beat or lane not in texts:
                continue
            state = drum_state(hit.velocity)
            texts[lane][step] = state
            quantized = section.start + (beat + step / division) * beat_duration
            diagnostics.append({
                "time": hit.time,
                "lane": hit.lane,
                "state": state,
                "velocity": hit.velocity,
                "confidence": hit.confidence,
                "energy_ratio": hit.energy_ratio,
                "provenance": hit.provenance,
                "quantized_time": quantized,
                "residual_ms": round((hit.time - quantized) * 1000.0, 3),
            })
        patterns.append({"division": division, "lanes": ["".join(texts[lane]) for lane in lanes]})
    return patterns, diagnostics


def midi_name(midi: int) -> str:
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[midi % 12]}{midi // 12 - 1}"


SUPPORTED_QUALITY_INTERVALS = {
    "": {0, 4, 7}, "m": {0, 3, 7}, "5": {0, 7}, "sus2": {0, 2, 7},
    "sus4": {0, 5, 7}, "dim": {0, 3, 6}, "aug": {0, 4, 8},
    "6": {0, 4, 7, 9}, "m6": {0, 3, 7, 9}, "7": {0, 4, 7, 10},
    "maj7": {0, 4, 7, 11}, "m7": {0, 3, 7, 10}, "m7b5": {0, 3, 6, 10},
    "dim7": {0, 3, 6, 9}, "add9": {0, 2, 4, 7}, "madd9": {0, 2, 3, 7},
    "9": {0, 2, 4, 7, 10}, "maj9": {0, 2, 4, 7, 11}, "m9": {0, 2, 3, 7, 10},
    "13": {0, 2, 4, 7, 9, 10}, "7b9": {0, 1, 4, 7, 10},
    "7#9": {0, 3, 4, 7, 10}, "#11": {0, 4, 6, 7},
    "maj7#11": {0, 4, 6, 7, 11}, "maj9#11": {0, 2, 4, 6, 7, 11},
}

QUALITY_ALIASES = {
    "maj": "", "major": "", "min": "m", "minor": "m", "-": "m",
    "+": "aug", "sus": "sus4", "dom7": "7", "ma7": "maj7", "min7": "m7",
    "hdim7": "m7b5", "min7b5": "m7b5", "min6": "m6", "min9": "m9",
    "minadd9": "madd9", "maj6": "6",
}


def normalize_chord(raw: str) -> tuple[str, bool]:
    value = raw.strip()
    if not value or value.upper() in {"N", "X", "NO_CHORD"}:
        return "-", value not in {"", "N"}
    bass = ""
    if "/" in value:
        value, bass_note = value.split("/", 1)
        bass = "/" + bass_note.replace(":", "").strip()
    if ":" in value:
        root, quality = value.split(":", 1)
    else:
        match = __import__("re").match(r"^([A-Ga-g](?:#|b)?)(.*)$", value)
        if not match:
            return "-", True
        root, quality = match.groups()
    root = root[:1].upper() + root[1:]
    quality = quality.lower().replace(" ", "")
    quality = QUALITY_ALIASES.get(quality, quality)
    if quality in SUPPORTED_QUALITY_INTERVALS:
        return root + quality + bass, False
    # ChordMini occasionally emits parenthesized interval sets. Approximate those
    # to Jam2's nearest supported quality, retaining the raw label in analysis.json.
    interval_aliases = {
        "1,3,5": {0, 4, 7}, "1,b3,5": {0, 3, 7}, "1,3,5,b7": {0, 4, 7, 10},
        "1,b3,5,b7": {0, 3, 7, 10}, "1,b3,b5,b7": {0, 3, 6, 10},
    }
    intervals = interval_aliases.get(quality.strip("()"))
    if intervals is None:
        # Deterministic conservative fallbacks for exotic vocabulary labels.
        intervals = {0, 3, 7} if quality.startswith("m") else {0, 4, 7}
    nearest = min(
        SUPPORTED_QUALITY_INTERVALS,
        key=lambda candidate: (
            len(intervals.symmetric_difference(SUPPORTED_QUALITY_INTERVALS[candidate])),
            len(SUPPORTED_QUALITY_INTERVALS[candidate]),
            candidate,
        ),
    )
    return root + nearest + bass, True


PITCH_CLASS = {
    "C": 0, "B#": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3,
    "E": 4, "Fb": 4, "F": 5, "E#": 5, "F#": 6, "Gb": 6,
    "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11,
}
PITCH_CLASS_NAME = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def _normalized_pitch_set(label: str) -> tuple[str, set[int]] | None:
    normalized, _ = normalize_chord(label)
    if normalized == "-":
        return None
    chord = normalized.split("/", 1)[0]
    match = __import__("re").match(r"^([A-G](?:#|b)?)(.*)$", chord)
    if not match:
        return None
    root, quality = match.groups()
    intervals = SUPPORTED_QUALITY_INTERVALS.get(quality)
    if intervals is None:
        return None
    root_pc = PITCH_CLASS[root]
    return normalized, {(root_pc + interval) % 12 for interval in intervals}


def fuse_chords_with_bass(
    chords: list[TimedLabel], bass: list[NoteEvent], max_bass_age: float = 4.0
) -> tuple[list[TimedLabel], list[dict[str, object]]]:
    """Recover rootless chord names when the separated bass supplies the root.

    The fusion is deliberately conservative: it only changes a chord when the
    bass is not already one of its pitch classes and adding that bass produces
    a supported Jam2 chord with at most one pitch-class disagreement.
    """
    fused: list[TimedLabel] = []
    diagnostics: list[dict[str, object]] = []
    fusion_qualities = ("", "m", "7", "maj7", "m7", "dim", "m7b5")
    for chord in chords:
        parsed = _normalized_pitch_set(chord.label)
        anchor = min(chord.end, chord.start + 0.15)
        preceding = [note for note in bass if note.start <= anchor and anchor - note.start <= max_bass_age]
        bass_note = max(preceding, key=lambda note: note.start) if preceding else None
        if parsed is None or bass_note is None or bass_note.midi % 12 in parsed[1]:
            fused.append(chord)
            continue
        root_pc = bass_note.midi % 12
        observed = set(parsed[1]) | {root_pc}
        quality = min(
            fusion_qualities,
            key=lambda candidate: (
                len(observed.symmetric_difference({
                    (root_pc + interval) % 12 for interval in SUPPORTED_QUALITY_INTERVALS[candidate]
                })),
                len(SUPPORTED_QUALITY_INTERVALS[candidate]),
                candidate,
            ),
        )
        candidate_set = {
            (root_pc + interval) % 12 for interval in SUPPORTED_QUALITY_INTERVALS[quality]
        }
        disagreement = len(observed.symmetric_difference(candidate_set))
        if disagreement > 1:
            fused.append(chord)
            continue
        label = PITCH_CLASS_NAME[root_pc] + quality
        fused_chord = TimedLabel(chord.start, chord.end, label, chord.confidence)
        fused.append(fused_chord)
        diagnostics.append({
            "start": chord.start,
            "end": chord.end,
            "upper_structure": chord.label,
            "bass_midi": bass_note.midi,
            "fused": label,
            "pitch_class_disagreement": disagreement,
        })
    return fused, diagnostics


def stabilize_chords(
    chords: list[TimedLabel], duration: float, maximum_gap: float = 0.25,
    maximum_fragment: float = 0.75,
) -> list[TimedLabel]:
    """Canonicalize labels and remove short same-root vocabulary fragments."""
    canonical = [
        TimedLabel(item.start, item.end, normalize_chord(item.label)[0], item.confidence)
        for item in chords if item.end > item.start
    ]
    for index, item in enumerate(canonical):
        if item.end - item.start > maximum_fragment:
            continue
        item_parts = _normalized_pitch_set(item.label)
        neighbors = []
        if index:
            neighbors.append(canonical[index - 1])
        if index + 1 < len(canonical):
            neighbors.append(canonical[index + 1])
        for neighbor in neighbors:
            neighbor_parts = _normalized_pitch_set(neighbor.label)
            if item_parts and neighbor_parts:
                item_root = _chord_root_pc(item.label)
                neighbor_root = _chord_root_pc(neighbor.label)
                if item_root == neighbor_root:
                    canonical[index] = TimedLabel(item.start, item.end, neighbor.label, item.confidence)
                    break
    if not canonical:
        return canonical
    result: list[TimedLabel] = []
    for index, item in enumerate(canonical):
        start = item.start
        end = item.end
        if index == 0 and start <= maximum_gap:
            start = 0.0
        if index + 1 < len(canonical):
            gap = canonical[index + 1].start - end
            if 0.0 < gap <= maximum_gap:
                end = canonical[index + 1].start
        elif 0.0 < duration - end <= maximum_gap:
            end = duration
        result.append(TimedLabel(start, end, item.label, item.confidence))
    return result


def _chord_root_pc(label: str) -> int | None:
    normalized, _ = normalize_chord(label)
    match = __import__("re").match(r"^([A-G](?:#|b)?)", normalized)
    return PITCH_CLASS.get(match.group(1)) if match else None


def _empty_step(state: str = "rest") -> dict[str, object]:
    return {"state": state, "value": "", "velocity": 88, "articulation": "", "voicing": ""}


def quantize_musical(
    chords: list[TimedLabel], bass: list[NoteEvent], section: SectionChoice,
    beats: list[float], bpm: float, division: int = 4,
) -> tuple[list[dict[str, object]], list[str], list[dict[str, object]]]:
    beat_duration = 60.0 / bpm
    # Use the source recording's tracked beat intervals. A uniform BPM grid is
    # the destination, but sampling the source with that same uniform grid lets
    # small live/analysis tempo offsets accumulate across a section.
    source_beats: list[float] = []
    for beat_offset in range(section.beats + 1):
        index = section.first_beat + beat_offset
        if 0 <= index < len(beats):
            source_beats.append(beats[index])
        elif source_beats:
            source_beats.append(source_beats[-1] + beat_duration)
        else:
            source_beats.append(section.start + beat_offset * beat_duration)
    if source_beats:
        source_beats[0] = section.start
        source_beats[-1] = section.end

    total_steps = section.beats * division

    def source_position(timestamp: float) -> float:
        if timestamp <= source_beats[0]:
            return 0.0
        if timestamp >= source_beats[-1]:
            return float(total_steps)
        beat_index = max(0, min(
            section.beats - 1, bisect_right(source_beats, timestamp) - 1
        ))
        interval = max(1e-6, source_beats[beat_index + 1] - source_beats[beat_index])
        return (beat_index + (timestamp - source_beats[beat_index]) / interval) * division

    def source_time(step: int) -> float:
        beat_index, subdivision = divmod(max(0, min(total_steps, step)), division)
        if beat_index >= section.beats:
            return source_beats[-1]
        fraction = subdivision / division
        return source_beats[beat_index] + fraction * (
            source_beats[beat_index + 1] - source_beats[beat_index]
        )

    chord_cells: list[TimedLabel | None] = [None] * total_steps
    chord_onsets: set[int] = set()
    bass_cells: list[NoteEvent | None] = [None] * total_steps
    bass_onsets: set[int] = set()
    diagnostics: list[dict[str, object]] = []

    for chord in chords:
        if chord.end <= section.start or chord.start >= section.end:
            continue
        first = max(0, min(total_steps - 1, int(round(float(
            source_position(max(chord.start, section.start))
        )))))
        last = max(first + 1, min(total_steps, int(round(float(
            source_position(min(chord.end, section.end))
        )))))
        for cell in range(first, last):
            current = chord_cells[cell]
            if current is None or (chord.confidence or 0.0) > (current.confidence or 0.0):
                chord_cells[cell] = chord
        chord_onsets.add(first)
        quantized = source_time(first)
        normalized, approximated = normalize_chord(chord.label)
        diagnostics.append({
            "kind": "chord", "time": float(chord.start), "quantized_time": float(quantized),
            "residual_ms": round(float(chord.start - quantized) * 1000.0, 3),
            "raw": chord.label, "jam2": normalized, "approximated": approximated,
        })

    for note in bass:
        if note.end <= section.start or note.start >= section.end:
            continue
        first = max(0, min(total_steps - 1, int(round(float(
            source_position(max(note.start, section.start))
        )))))
        last = max(first + 1, min(total_steps, int(round(float(
            source_position(min(note.end, section.end))
        )))))
        for cell in range(first, last):
            current = bass_cells[cell]
            if current is None or note.midi < current.midi:
                bass_cells[cell] = note
        bass_onsets.add(first)
        quantized = source_time(first)
        diagnostics.append({
            "kind": "bass", "time": float(note.start), "quantized_time": float(quantized),
            "residual_ms": round(float(note.start - quantized) * 1000.0, 3),
            "midi": int(note.midi), "jam2": midi_name(note.midi),
        })

    patterns: list[dict[str, object]] = []
    legacy: list[str] = []
    previous_chord = "-"
    previous_bass: int | None = None
    for beat in range(section.beats):
        chord_steps: list[dict[str, object]] = []
        bass_steps: list[dict[str, object]] = []
        for step in range(division):
            cell = beat * division + step
            chord_event = chord_cells[cell]
            normalized, approximated = normalize_chord(chord_event.label if chord_event else "N")
            if normalized == "-":
                chord_step = _empty_step("rest")
            elif normalized != previous_chord or cell in chord_onsets:
                chord_step = _empty_step("onset")
                chord_step["value"] = normalized
            else:
                chord_step = _empty_step("hold")
            chord_steps.append(chord_step)
            previous_chord = normalized

            active = bass_cells[cell]
            if active is None:
                bass_step = _empty_step("rest")
                previous_bass = None
            elif active.midi != previous_bass or cell in bass_onsets:
                bass_step = _empty_step("onset")
                bass_step["value"] = midi_name(active.midi)
                bass_step["velocity"] = max(1, min(127, active.velocity))
                previous_bass = active.midi
            else:
                bass_step = _empty_step("hold")
            bass_steps.append(bass_step)
        onsets = [str(step["value"]) for step in chord_steps if step["state"] == "onset"]
        legacy.append(onsets[0] if onsets else ("-" if all(step["state"] == "rest" for step in chord_steps) else ""))
        patterns.append({
            "division": division,
            "chords": chord_steps,
            "melody": [_empty_step("rest") for _ in range(division)],
            "bass": bass_steps,
            "support": [_empty_step("rest") for _ in range(division)],
        })
    return patterns, legacy, diagnostics
