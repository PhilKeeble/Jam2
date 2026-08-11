from __future__ import annotations

from collections import Counter, defaultdict
from array import array
from dataclasses import replace
import math
from pathlib import Path
import statistics
from typing import Iterable
import wave

from .timing import normalize_chord
from .types import DrumHit, NoteEvent, SectionChoice, TimedLabel


PITCH_CLASSES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
ROOT_TO_PC = {name: index for index, name in enumerate(PITCH_CLASSES)}
ROOT_TO_PC.update({"Db": 1, "Eb": 3, "Gb": 6, "Ab": 8, "Bb": 10})
MAJOR_PROFILE = (6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88)
MINOR_PROFILE = (6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17)


def _feature_distance(left: Counter[str], right: Counter[str]) -> float:
    keys = set(left) | set(right)
    if not keys:
        return 0.0
    numerator = sum(float(left[key]) * float(right[key]) for key in keys)
    left_norm = math.sqrt(sum(float(value) ** 2 for value in left.values()))
    right_norm = math.sqrt(sum(float(value) ** 2 for value in right.values()))
    if left_norm == 0.0 or right_norm == 0.0:
        return 0.0 if left_norm == right_norm else 1.0
    return max(0.0, min(1.0, 1.0 - numerator / (left_norm * right_norm)))


def _mean_features(rows: list[Counter[str]]) -> Counter[str]:
    combined: Counter[str] = Counter()
    if not rows:
        return combined
    for row in rows:
        combined.update(row)
    scale = 1.0 / len(rows)
    return Counter({key: value * scale for key, value in combined.items()})


def _bar_rms(path: Path, bounds: list[tuple[float, float]]) -> list[float]:
    if not path.is_file():
        return [0.0] * len(bounds)
    try:
        with wave.open(str(path), "rb") as source:
            if source.getsampwidth() != 2:
                return [0.0] * len(bounds)
            rate = source.getframerate()
            channels = source.getnchannels()
            result = []
            for start, end in bounds:
                first = max(0, min(source.getnframes(), round(start * rate)))
                count = max(0, min(source.getnframes() - first, round((end - start) * rate)))
                source.setpos(first)
                samples = array("h")
                samples.frombytes(source.readframes(count))
                if channels > 1:
                    values = samples[::channels]
                else:
                    values = samples
                if not values:
                    result.append(0.0)
                    continue
                mean_square = sum(float(value) * float(value) for value in values) / len(values)
                result.append(20.0 * math.log10(max(1.0, math.sqrt(mean_square)) / 32768.0))
            return result
    except (OSError, EOFError, wave.Error):
        return [0.0] * len(bounds)


def infer_song_sections(
    beats: list[float],
    downbeats: list[float],
    beats_per_bar: int,
    chords: list[TimedLabel],
    drums: list[DrumHit],
    bass: list[NoteEvent],
    duration: float,
    energy_paths: dict[str, Path] | None = None,
    minimum_bars: int = 4,
    maximum_sections: int = 11,
) -> tuple[list[TimedLabel], dict[str, object]]:
    """Infer conservative anonymous sections from sustained musical change.

    Boundaries are restricted to tracked downbeats. Two-bar context on either
    side and a four-bar minimum prevent fills, pickup notes and AAB-style bar
    variations from becoming tiny sections.
    """
    if len(beats) < beats_per_bar * 2:
        return [TimedLabel(0.0, duration, "Section A")], {
            "enabled": False, "reason": "insufficient tracked beats",
        }

    def nearest_beat(value: float) -> int:
        return min(range(len(beats)), key=lambda index: abs(beats[index] - value))

    indices = sorted({nearest_beat(value) for value in downbeats})
    indices = [value for value in indices if 0 <= value < len(beats)]
    if len(indices) < 3:
        indices = list(range(0, len(beats), beats_per_bar))
    clean = [indices[0]] if indices else [0]
    for value in indices[1:]:
        if value - clean[-1] >= max(2, beats_per_bar - 1):
            clean.append(value)
    bounds = [
        (beats[first], min(duration, beats[last]))
        for first, last in zip(clean, clean[1:])
        if beats[last] > beats[first]
    ]
    bar_count = len(bounds)
    if bar_count < minimum_bars * 2:
        return [TimedLabel(bounds[0][0] if bounds else 0.0,
                           bounds[-1][1] if bounds else duration,
                           "Section A")], {
            "enabled": False, "reason": "fewer than eight complete bars",
            "bars": bar_count,
        }

    chord_rows: list[Counter[str]] = []
    drum_rows: list[Counter[str]] = []
    bass_rows: list[Counter[str]] = []
    for start, end in bounds:
        chord_features: Counter[str] = Counter()
        labels: list[str] = []
        for offset in range(beats_per_bar):
            timestamp = start + (end - start) * (offset + 0.5) / beats_per_bar
            event = _label_at(chords, timestamp)
            label = normalize_chord(event.label)[0] if event else "-"
            labels.append(label)
            chord_features[f"position:{offset}:{label}"] += 1.4
            chord_features[f"chord:{label}"] += 0.5
        for previous, current in zip(labels, labels[1:]):
            chord_features[f"cadence:{previous}>{current}"] += 0.8
        chord_rows.append(chord_features)

        drum_features: Counter[str] = Counter()
        for hit in drums:
            if not start <= hit.time < end:
                continue
            slot = min(15, max(0, round((hit.time - start) / (end - start) * 16)))
            drum_features[f"{hit.lane}:{slot}"] += 0.6 + hit.velocity / 127.0
            drum_features[f"lane:{hit.lane}"] += 0.20
        drum_rows.append(drum_features)

        bass_features: Counter[str] = Counter()
        for note in bass:
            if not start <= note.start < end:
                continue
            slot = min(7, max(0, round((note.start - start) / (end - start) * 8)))
            bass_features[f"pitch:{note.midi % 12}"] += 0.6
            bass_features[f"position:{slot}:{note.midi % 12}"] += 0.8
        bass_rows.append(bass_features)

    energy = {
        name: _bar_rms(Path(path), bounds)
        for name, path in (energy_paths or {}).items()
    }
    candidates: list[dict[str, object]] = []
    context_bars = 2
    for index in range(minimum_bars, bar_count - minimum_bars + 1):
        before = slice(max(0, index - context_bars), index)
        after = slice(index, min(bar_count, index + context_bars))
        chord_change = _feature_distance(
            _mean_features(chord_rows[before]), _mean_features(chord_rows[after]))
        drum_change = _feature_distance(
            _mean_features(drum_rows[before]), _mean_features(drum_rows[after]))
        bass_change = _feature_distance(
            _mean_features(bass_rows[before]), _mean_features(bass_rows[after]))
        energy_changes = []
        for values in energy.values():
            left = statistics.mean(values[before])
            right = statistics.mean(values[after])
            energy_changes.append(min(1.0, abs(right - left) / 12.0))
        energy_change = statistics.mean(energy_changes) if energy_changes else 0.0
        phrase_bonus = 0.06 if index % 4 == 0 else 0.02 if index % 2 == 0 else 0.0
        score = (
            0.43 * chord_change + 0.36 * drum_change +
            0.13 * bass_change + 0.08 * energy_change + phrase_bonus
        )
        modalities = sum(value >= 0.24 for value in (
            chord_change, drum_change, bass_change, energy_change
        ))
        clear = (score >= 0.36 and modalities >= 2) or score >= 0.58
        candidates.append({
            "bar": index,
            "time": bounds[index][0],
            "score": round(score, 6),
            "chord_change": round(chord_change, 6),
            "drum_change": round(drum_change, 6),
            "bass_change": round(bass_change, 6),
            "energy_change": round(energy_change, 6),
            "clear": clear,
        })

    selected: list[int] = []
    for candidate in sorted(
        (value for value in candidates if bool(value["clear"])),
        key=lambda value: float(value["score"]),
        reverse=True,
    ):
        bar = int(candidate["bar"])
        if all(abs(bar - existing) >= minimum_bars for existing in selected):
            selected.append(bar)
        if len(selected) >= maximum_sections - 1:
            break
    selected.sort()

    # Jam2 sections cannot exceed 512 beats. Add only the minimum technical
    # boundaries needed for unusually long, otherwise-uniform recordings.
    maximum_bars = max(1, 512 // beats_per_bar)
    complete = [0, *selected, bar_count]
    for first, last in list(zip(complete, complete[1:])):
        cursor = first + maximum_bars
        while cursor < last and len(selected) < maximum_sections - 1:
            selected.append(cursor - cursor % 4)
            cursor += maximum_bars
    selected = sorted(set(value for value in selected if 0 < value < bar_count))

    section_bars = [0, *selected, bar_count]
    structures = [
        TimedLabel(
            bounds[first][0],
            bounds[last - 1][1],
            f"Section {chr(ord('A') + index)}",
            next((float(item["score"]) for item in candidates
                  if int(item["bar"]) == last), None),
        )
        for index, (first, last) in enumerate(zip(section_bars, section_bars[1:]))
    ]
    return structures, {
        "enabled": True,
        "method": "bar_aligned_multimodal_change_points",
        "bars": bar_count,
        "minimum_section_bars": minimum_bars,
        "candidates": candidates,
        "selected_boundary_bars": selected,
        "sections": structures,
    }


def _root_pc(label: str) -> int | None:
    normalized, _ = normalize_chord(label)
    if normalized == "-":
        return None
    root = normalized[0] + (normalized[1] if len(normalized) > 1 and normalized[1] in "#b" else "")
    return ROOT_TO_PC.get(root)


def _label_at(events: Iterable[TimedLabel], timestamp: float) -> TimedLabel | None:
    return next((event for event in events if event.start <= timestamp < event.end), None)


def estimate_key(chroma_rows: list[dict[str, object]]) -> dict[str, object]:
    """Estimate a soft key prior from beat-synchronous chroma evidence."""
    profiles = [row.get("profile") for row in chroma_rows]
    profiles = [value for value in profiles if isinstance(value, list) and len(value) == 12]
    if not profiles:
        return {"label": None, "confidence_margin": 0.0, "scores": []}
    aggregate = [sum(float(profile[index]) for profile in profiles) for index in range(12)]
    mean = sum(aggregate) / 12.0
    centered = [value - mean for value in aggregate]

    def correlation(template: tuple[float, ...], root: int) -> float:
        rotated = [template[(index - root) % 12] for index in range(12)]
        template_mean = sum(rotated) / 12.0
        shifted = [value - template_mean for value in rotated]
        denominator = math.sqrt(sum(value * value for value in centered) * sum(value * value for value in shifted))
        return sum(a * b for a, b in zip(centered, shifted)) / denominator if denominator else 0.0

    scores = []
    for root in range(12):
        scores.append((correlation(MAJOR_PROFILE, root), PITCH_CLASSES[root]))
        scores.append((correlation(MINOR_PROFILE, root), PITCH_CLASSES[root] + "m"))
    scores.sort(reverse=True)
    return {
        "label": scores[0][1],
        "confidence_margin": scores[0][0] - scores[1][0],
        "scores": [{"label": label, "score": round(score, 6)} for score, label in scores[:5]],
    }


def _key_membership(label: str, key_label: str | None) -> float:
    if not key_label:
        return 0.0
    chord_root = _root_pc(label)
    if chord_root is None:
        return 0.0
    key_minor = key_label.endswith("m")
    key_root = ROOT_TO_PC[key_label[:-1] if key_minor else key_label]
    scale = {0, 2, 3, 5, 7, 8, 10} if key_minor else {0, 2, 4, 5, 7, 9, 11}
    return 0.16 if (chord_root - key_root) % 12 in scale else -0.10


def _bass_support(label: str, bass: list[NoteEvent], start: float, end: float) -> float:
    root = _root_pc(label)
    if root is None:
        return 0.0
    active = [note for note in bass if note.start < end and note.end > start]
    if not active:
        active = [note for note in bass if start - 0.12 <= note.start < end + 0.12]
    if not active:
        return 0.0
    pitch_classes = [note.midi % 12 for note in active]
    if root in pitch_classes:
        return 0.28
    return -0.05


def contextualize_chords(
    primary: list[TimedLabel],
    chroma_layers: list[list[dict[str, object]]],
    bass: list[NoteEvent],
    beats: list[float],
    structures: list[TimedLabel],
    duration: float,
) -> tuple[list[TimedLabel], dict[str, object]]:
    """Decode chord evidence with soft key, bass, continuity and repetition priors."""
    if not beats:
        return primary, {"enabled": False, "reason": "no beats"}
    boundaries = list(beats)
    if boundaries[-1] < duration:
        interval = statistics.median(b - a for a, b in zip(beats, beats[1:])) if len(beats) > 1 else 0.5
        boundaries.append(min(duration, beats[-1] + interval))
    if len(boundaries) < 2:
        return primary, {"enabled": False, "reason": "fewer than two beat boundaries"}

    key = estimate_key(chroma_layers[0] if chroma_layers else [])
    cell_candidates: list[dict[str, float]] = []
    for start, end in zip(boundaries, boundaries[1:]):
        midpoint = (start + end) * 0.5
        scores: defaultdict[str, float] = defaultdict(float)
        event = _label_at(primary, midpoint)
        if event is not None:
            label, _ = normalize_chord(event.label)
            if label != "-":
                scores[label] += 1.80
        for layer_index, layer in enumerate(chroma_layers):
            row = next((item for item in layer if float(item["start"]) <= midpoint < float(item["end"])), None)
            if row is None:
                continue
            candidates = row.get("candidates")
            if not isinstance(candidates, list):
                candidates = [{"label": row.get("label", "N"), "score": 0.0}]
            for rank, candidate in enumerate(candidates[:3]):
                label, _ = normalize_chord(str(candidate.get("label", "N")))
                if label == "-":
                    continue
                base = (0.72 if layer_index == 0 else 0.56) * (1.0, 0.40, 0.18)[rank]
                scores[label] += base
        for label in list(scores):
            scores[label] += _key_membership(label, key.get("label"))
            scores[label] += _bass_support(label, bass, start, end)
        if not scores:
            scores["-"] = 0.0
        cell_candidates.append(dict(scores))

    # Small-state Viterbi decoding prevents a single lead note from causing a
    # rapid chord excursion while still allowing supported beat-level changes.
    states = sorted({label for scores in cell_candidates for label in scores})
    costs: list[dict[str, float]] = []
    back: list[dict[str, str | None]] = []
    for index, emissions in enumerate(cell_candidates):
        row_cost: dict[str, float] = {}
        row_back: dict[str, str | None] = {}
        for state in states:
            emission = emissions.get(state, -0.85)
            if index == 0:
                row_cost[state] = emission
                row_back[state] = None
                continue
            previous, score = max(
                costs[-1].items(),
                key=lambda item: item[1] + (0.22 if item[0] == state else -0.20),
            )
            row_cost[state] = score + (0.22 if previous == state else -0.20) + emission
            row_back[state] = previous
        costs.append(row_cost)
        back.append(row_back)
    selected = [max(costs[-1], key=costs[-1].get)]
    for index in range(len(costs) - 1, 0, -1):
        selected.append(str(back[index][selected[-1]]))
    selected.reverse()
    primary_cells = []
    for start, end in zip(boundaries, boundaries[1:]):
        event = _label_at(primary, (start + end) * 0.5)
        primary_cells.append(normalize_chord(event.label)[0] if event else "-")
    decoder_changes = sum(a != b for a, b in zip(primary_cells, selected))

    corrections: list[dict[str, object]] = []
    # Same-labelled structural repetitions contribute a soft aligned template.
    groups: defaultdict[str, list[tuple[int, int]]] = defaultdict(list)
    for structure in structures:
        indices = [i for i, (a, b) in enumerate(zip(boundaries, boundaries[1:])) if a >= structure.start - 0.08 and b <= structure.end + 0.08]
        if indices:
            groups[structure.label.strip().lower()].append((indices[0], indices[-1] + 1))
    for label, spans in groups.items():
        if len(spans) < 2:
            continue
        common = min(end - start for start, end in spans)
        for offset in range(common):
            indices = [start + offset for start, _ in spans]
            votes = Counter(selected[index] for index in indices)
            consensus, count = votes.most_common(1)[0]
            if count < 2:
                continue
            for index in indices:
                if selected[index] == consensus:
                    continue
                own = cell_candidates[index].get(selected[index], -1.0)
                supported = cell_candidates[index].get(consensus, -1.0)
                if supported >= own - 0.38:
                    corrections.append({"beat": index, "from": selected[index], "to": consensus, "reason": f"repeated {label} consensus"})
                    selected[index] = consensus

    # A one-beat excursion surrounded by the same chord is usually melody or
    # separation leakage. Preserve it when its direct evidence is decisively stronger.
    for index in range(1, len(selected) - 1):
        if selected[index - 1] != selected[index + 1] or selected[index] == selected[index - 1]:
            continue
        keep = cell_candidates[index].get(selected[index], -1.0)
        replace_score = cell_candidates[index].get(selected[index - 1], -1.0)
        if keep < replace_score + 1.10:
            corrections.append({"beat": index, "from": selected[index], "to": selected[index - 1], "reason": "isolated one-beat excursion"})
            selected[index] = selected[index - 1]

    result: list[TimedLabel] = []
    for index, label in enumerate(selected):
        start, end = boundaries[index], boundaries[index + 1]
        confidence = None
        if result and result[-1].label == label:
            result[-1] = replace(result[-1], end=end)
        else:
            result.append(TimedLabel(start, end, label, confidence))
    return result, {
        "enabled": True,
        "key": key,
        "beat_cells": len(selected),
        "input_segments": len(primary),
        "output_segments": len(result),
        "decoder_changes": decoder_changes,
        "corrections": corrections,
    }


def contextualize_bass(
    notes: list[NoteEvent], chords: list[TimedLabel], min_duration: float = 0.055
) -> tuple[list[NoteEvent], list[dict[str, object]]]:
    """Make the Basic Pitch result monophonic without inventing missing notes."""
    if not notes:
        return [], []
    ordered = sorted(notes, key=lambda note: (note.start, note.midi))
    result: list[NoteEvent] = []
    diagnostics: list[dict[str, object]] = []
    for note in ordered:
        if note.end - note.start < min_duration:
            diagnostics.append({"action": "remove", "reason": "short fragment", "threshold_seconds": min_duration, "note": note.midi, "time": note.start})
            continue
        if result and abs(note.start - result[-1].start) <= 0.055:
            previous = result[-1]
            chord = _label_at(chords, note.start)
            root = _root_pc(chord.label) if chord else None
            previous_score = (0.3 if previous.midi % 12 == root else 0.0) + previous.velocity / 127.0
            note_score = (0.3 if note.midi % 12 == root else 0.0) + note.velocity / 127.0
            keep = note if note_score > previous_score else previous
            discard = previous if keep is note else note
            result[-1] = keep
            diagnostics.append({"action": "remove", "reason": "simultaneous bass candidate", "note": discard.midi, "time": discard.start})
            continue
        if result and note.start < result[-1].end:
            previous = result[-1]
            result[-1] = replace(previous, end=max(previous.start + 0.03, note.start))
            diagnostics.append({"action": "trim", "reason": "monophonic overlap", "note": previous.midi, "end": note.start})
        result.append(note)
    return result, diagnostics


def classify_cymbals(
    drums: list[DrumHit], beats: list[float], downbeats: list[float], structures: list[TimedLabel]
) -> tuple[list[DrumHit], list[dict[str, object]]]:
    """Separate sustained ride-like cymbal streams from boundary crash accents."""
    if not beats:
        return drums, []
    median_beat = statistics.median(b - a for a, b in zip(beats, beats[1:])) if len(beats) > 1 else 0.5
    regions = structures or [TimedLabel(beats[0], beats[-1] + median_beat, "song")]
    replacements: dict[int, DrumHit] = {}
    diagnostics: list[dict[str, object]] = []
    for region in regions:
        indexed = [(index, hit) for index, hit in enumerate(drums) if hit.lane == "Crash" and region.start <= hit.time < region.end]
        if len(indexed) < 5:
            continue
        intervals = [b.time - a.time for (_, a), (_, b) in zip(indexed, indexed[1:])]
        regular = [value for value in intervals if 0.20 * median_beat <= value <= 1.35 * median_beat]
        if len(regular) < max(3, math.ceil(len(intervals) * 0.55)):
            continue
        for index, hit in indexed:
            boundary = any(abs(hit.time - value) <= min(0.10, median_beat * 0.16) for value in downbeats)
            if boundary and hit.velocity >= 91:
                continue
            replacements[index] = replace(hit, lane="Ride", provenance=hit.provenance + "_ride_context")
            diagnostics.append({"time": hit.time, "from": "Crash", "to": "Ride", "section": region.label, "reason": "sustained regular cymbal pattern"})
    return [replacements.get(index, hit) for index, hit in enumerate(drums)], diagnostics


def section_timing_diagnostics(
    choices: list[SectionChoice], beats: list[float], bpm: float
) -> dict[str, dict[str, object]]:
    """Expose local residuals hidden by endpoint-only section stretching."""
    result: dict[str, dict[str, object]] = {}
    ideal_beat = 60.0 / bpm
    for bank, choice in enumerate(choices):
        source_duration = choice.end - choice.start
        target_duration = choice.beats * ideal_beat
        factor = target_duration / source_duration if source_duration > 0.0 else 1.0
        rows = []
        for beat_index in range(choice.first_beat, min(len(beats), choice.first_beat + choice.beats + 1)):
            source_relative = beats[beat_index] - choice.start
            ideal_relative = (beat_index - choice.first_beat) * ideal_beat
            residual_ms = (source_relative * factor - ideal_relative) * 1000.0
            rows.append({"beat": beat_index - choice.first_beat, "source_seconds": source_relative, "residual_ms": round(residual_ms, 3)})
        absolute = [abs(float(row["residual_ms"])) for row in rows]
        result[chr(ord("A") + bank)] = {
            "source_seconds": source_duration,
            "target_seconds": target_duration,
            "global_stretch_factor": factor,
            "mean_absolute_residual_ms": round(sum(absolute) / len(absolute), 3) if absolute else 0.0,
            "max_absolute_residual_ms": round(max(absolute), 3) if absolute else 0.0,
            "beats": rows,
        }
    return result
