from __future__ import annotations

from array import array
import json
import math
from pathlib import Path
import wave

from .paths import REPORTS_ROOT, portable_slug
from .timing import drum_state, normalize_chord


def _read_mono_pcm16(path: Path, frames: int) -> tuple[int, array]:
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise ValueError(f"benchmark asset is not mono PCM16: {path}")
        if source.getnframes() < frames:
            raise ValueError(f"benchmark asset is shorter than the requested excerpt: {path}")
        sample_rate = source.getframerate()
        values = array("h")
        values.frombytes(source.readframes(frames))
    return sample_rate, values


def _write_mono_pcm16(path: Path, sample_rate: int, values: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = array("h", (max(-32768, min(32767, value)) for value in values))
    with wave.open(str(path), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(sample_rate)
        target.writeframes(pcm.tobytes())


def prepare_generated_benchmark(
    jamjar_path: Path, name: str, bank_index: int, bars: int
) -> tuple[Path, Path]:
    if bars < 1:
        raise ValueError("--bars must be at least one")
    jamjar_path = jamjar_path.resolve()
    document = json.loads(jamjar_path.read_text(encoding="utf-8"))
    banks = document.get("looper", {}).get("banks", [])
    sections = document.get("sections", [])
    if not 0 <= bank_index < min(len(banks), len(sections)):
        raise ValueError(f"bank index {bank_index} does not exist")
    bank = banks[bank_index]
    section = sections[bank_index]
    recipe = section.get("generated_recipe")
    if not isinstance(recipe, dict):
        raise ValueError("selected bank is not backed by a Jam2 generated recipe")
    bpm = float(recipe["bpm"])
    beats_per_bar = int(recipe["beats_per_bar"])
    if bars > int(recipe["bars"]):
        raise ValueError(f"--bars exceeds the generated section's {recipe['bars']} bars")

    lanes: dict[str, Path] = {}
    for lane in bank.get("lanes", []):
        kind = str(lane.get("reference_kind", ""))
        if kind in {"chord", "drum", "melody", "bass"}:
            lanes[kind] = jamjar_path.parent / Path(str(lane["asset_path"]))
    missing = {"chord", "drum", "bass"} - lanes.keys()
    if missing:
        raise ValueError(f"selected bank lacks generated truth assets: {', '.join(sorted(missing))}")

    first_rate, first = _read_mono_pcm16(lanes["chord"], 1)
    del first
    total_beats = bars * beats_per_bar
    duration = total_beats * 60.0 / bpm
    frames = round(duration * first_rate)
    source_values: dict[str, array] = {}
    for kind, path in lanes.items():
        sample_rate, values = _read_mono_pcm16(path, frames)
        if sample_rate != first_rate:
            raise ValueError("generated benchmark assets have mismatched sample rates")
        source_values[kind] = values
    if "melody" not in source_values:
        source_values["melody"] = array("h", [0]) * frames

    sums = [
        int(source_values["chord"][index])
        + int(source_values["drum"][index])
        + int(source_values["melody"][index])
        + int(source_values["bass"][index])
        for index in range(frames)
    ]
    peak = max(1, max(abs(value) for value in sums))
    gain = min(1.0, (0.95 * 32767.0) / peak)
    scaled = {
        kind: [round(int(value) * gain) for value in values]
        for kind, values in source_values.items()
    }
    truth_stems = {
        "drums": scaled["drum"],
        "bass": scaled["bass"],
        "other": [
            scaled["chord"][index] + scaled["melody"][index] for index in range(frames)
        ],
        "vocals": [0] * frames,
    }
    mix = [
        truth_stems["drums"][index]
        + truth_stems["bass"][index]
        + truth_stems["other"][index]
        for index in range(frames)
    ]

    slug = portable_slug(name)
    root = REPORTS_ROOT / "benchmarks" / slug
    if root.exists():
        raise FileExistsError(f"benchmark already exists; refusing to overwrite: {root}")
    input_path = root / "input" / "fullmix.wav"
    _write_mono_pcm16(input_path, first_rate, mix)
    for stem, values in truth_stems.items():
        _write_mono_pcm16(root / "truth" / f"{stem}.wav", first_rate, values)

    progression = recipe.get("progression", {}).get("base_harmony", [])
    chord_truth = [
        {
            "start": float(item["beat"]) * 60.0 / bpm,
            "end": min(duration, (float(item["beat"]) + float(item["duration_beats"])) * 60.0 / bpm),
            "label": str(item["chord"]),
        }
        for item in progression
        if float(item["beat"]) < total_beats
    ]
    drum_events = recipe.get("groove", {}).get("performance_events", [])
    all_ticks = [int(item["tick"]) for item in drum_events]
    ticks_per_beat = max(1, round((max(all_ticks, default=total_beats) + 1) / int(recipe["bars"]) / beats_per_bar))
    drum_truth = []
    for item in drum_events:
        tick = int(item["tick"])
        if tick >= total_beats * ticks_per_beat:
            continue
        drum_truth.append({
            "time": tick / ticks_per_beat * 60.0 / bpm + float(item.get("offset_ms", 0)) / 1000.0,
            "lane": str(item["lane"]),
            "velocity": int(item.get("velocity", 100)),
            "state": drum_state(int(item.get("velocity", 100))),
            "fill": bool(item.get("fill", False)),
        })
    bass_truth = []
    for item in recipe.get("roles", {}).get("bass_events", []):
        tick = int(item["tick"])
        if tick >= total_beats * ticks_per_beat:
            continue
        bass_truth.append({
            "start": tick / ticks_per_beat * 60.0 / bpm,
            "end": (tick + int(item["duration_ticks"])) / ticks_per_beat * 60.0 / bpm,
            "midi": int(item["midi"]),
            "velocity": int(item.get("velocity", 88)),
        })
    lane_names = (
        "Kick", "Snare", "Closed HH", "Open HH", "Ride", "Crash",
        "High Tom", "Mid Tom", "Floor Tom", "Cross-stick / Rim",
    )
    drum_grid = []
    for beat, pattern in enumerate(section.get("beat_patterns", [])[:total_beats]):
        division = int(pattern.get("division", 4))
        for lane_index, text in enumerate(pattern.get("lanes", [])):
            if lane_index >= len(lane_names):
                break
            for step, state in enumerate(str(text).lower()[:division]):
                if state not in {"g", "x", "a"}:
                    continue
                drum_grid.append({
                    "time": (beat + step / division) * 60.0 / bpm,
                    "beat": beat,
                    "step": step,
                    "division": division,
                    "lane": lane_names[lane_index],
                    "state": state,
                })
    fill_regions = [
        {
            "start_beat": int(item["fill_start_beat"]),
            "end_beat": int(item["fill_start_beat"]) + int(item["fill_beat_count"]),
            "start": int(item["fill_start_beat"]) * 60.0 / bpm,
            "end": (int(item["fill_start_beat"]) + int(item["fill_beat_count"])) * 60.0 / bpm,
            "label": str(item.get("label", "")),
            "fill_id": str(item.get("fill_id", "")),
        }
        for item in recipe.get("groove", {}).get("phrase_plan", [])
        if int(item["fill_start_beat"]) < total_beats
    ]
    truth = {
        "format": "jamtaster-generated-benchmark-v1",
        "name": name,
        "source_jamjar": str(jamjar_path),
        "bank_index": bank_index,
        "bars": bars,
        "bpm": bpm,
        "beats_per_bar": beats_per_bar,
        "sample_rate": first_rate,
        "frames": frames,
        "duration_seconds": frames / first_rate,
        "mix_gain": gain,
        "ticks_per_beat": ticks_per_beat,
        "beats": [beat * 60.0 / bpm for beat in range(total_beats)],
        "downbeats": [bar * beats_per_bar * 60.0 / bpm for bar in range(bars)],
        "chords": chord_truth,
        "drums": drum_truth,
        "drum_grid": drum_grid,
        "fill_regions": fill_regions,
        "bass": bass_truth,
        "truth_stems": {stem: str(root / "truth" / f"{stem}.wav") for stem in truth_stems},
        "source_assets": {kind: str(path.resolve()) for kind, path in lanes.items()},
    }
    truth_path = root / "truth.json"
    truth_path.write_text(json.dumps(truth, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return input_path, truth_path


def _read_float_pcm16(path: Path):
    import numpy as np

    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise ValueError(f"score asset is not mono PCM16: {path}")
        rate = source.getframerate()
        values = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2").astype(np.float64)
    return rate, values / 32768.0


def _audio_score(truth_path: Path, estimate_path: Path) -> dict[str, float | int | None]:
    import numpy as np

    truth_rate, truth = _read_float_pcm16(truth_path)
    estimate_rate, estimate = _read_float_pcm16(estimate_path)
    if truth_rate != estimate_rate:
        raise ValueError("truth and estimate sample rates differ")
    frames = min(len(truth), len(estimate))
    truth = truth[:frames]
    estimate = estimate[:frames]
    truth_energy = float(np.dot(truth, truth))
    estimate_energy = float(np.dot(estimate, estimate))
    correlation = None
    si_sdr = None
    if truth_energy > 1e-12 and estimate_energy > 1e-12:
        correlation = float(np.corrcoef(truth, estimate)[0, 1])
        scaled_truth = truth * (float(np.dot(estimate, truth)) / truth_energy)
        error = estimate - scaled_truth
        si_sdr = 10.0 * math.log10(
            max(1e-20, float(np.dot(scaled_truth, scaled_truth)))
            / max(1e-20, float(np.dot(error, error)))
        )
    rms = math.sqrt(estimate_energy / max(1, frames))
    return {
        "frames_compared": frames,
        "correlation": correlation,
        "si_sdr_db": si_sdr,
        "estimate_rms_dbfs": 20.0 * math.log10(max(1e-12, rms)),
    }


def _match_events(
    truth: list[tuple[float, str]], estimate: list[tuple[float, str]], tolerance: float
) -> dict[str, object]:
    remaining = set(range(len(estimate)))
    errors: list[float] = []
    matches = 0
    for truth_time, truth_label in truth:
        candidates = [
            index for index in remaining
            if estimate[index][1] == truth_label and abs(estimate[index][0] - truth_time) <= tolerance
        ]
        if not candidates:
            continue
        best = min(candidates, key=lambda index: abs(estimate[index][0] - truth_time))
        remaining.remove(best)
        matches += 1
        errors.append((estimate[best][0] - truth_time) * 1000.0)
    precision = matches / len(estimate) if estimate else 0.0
    recall = matches / len(truth) if truth else 0.0
    f1 = 2.0 * precision * recall / (precision + recall) if precision + recall else 0.0
    errors_sorted = sorted(errors)
    median = errors_sorted[len(errors_sorted) // 2] if errors_sorted else None
    return {
        "truth": len(truth), "estimated": len(estimate), "matched": matches,
        "precision": precision, "recall": recall, "f1": f1,
        "median_signed_error_ms": median,
        "mean_absolute_error_ms": sum(abs(value) for value in errors) / len(errors) if errors else None,
        "tolerance_ms": tolerance * 1000.0,
    }


def _match_drum_events(
    truth: list[dict[str, object]],
    estimate: list[dict[str, object]],
    tolerance: float,
) -> tuple[dict[str, object], list[tuple[int, int]]]:
    remaining = set(range(len(estimate)))
    pairs: list[tuple[int, int]] = []
    errors: list[float] = []
    for truth_index, truth_event in enumerate(truth):
        candidates = [
            estimate_index for estimate_index in remaining
            if estimate[estimate_index]["family"] == truth_event["family"]
            and abs(float(estimate[estimate_index]["time"]) - float(truth_event["time"])) <= tolerance
        ]
        if not candidates:
            continue
        best = min(
            candidates,
            key=lambda index: abs(float(estimate[index]["time"]) - float(truth_event["time"])),
        )
        remaining.remove(best)
        pairs.append((truth_index, best))
        errors.append(
            (float(estimate[best]["time"]) - float(truth_event["time"])) * 1000.0
        )
    matches = len(pairs)
    precision = matches / len(estimate) if estimate else 0.0
    recall = matches / len(truth) if truth else 0.0
    f1 = 2.0 * precision * recall / (precision + recall) if precision + recall else 0.0
    state_matches = sum(
        str(truth[truth_index].get("state", "x"))
        == str(estimate[estimate_index].get("state", "x"))
        for truth_index, estimate_index in pairs
    )
    confusion: dict[str, dict[str, int]] = {
        state: {other: 0 for other in ("g", "x", "a")} for state in ("g", "x", "a")
    }
    for truth_index, estimate_index in pairs:
        truth_state = str(truth[truth_index].get("state", "x"))
        estimate_state = str(estimate[estimate_index].get("state", "x"))
        if truth_state in confusion and estimate_state in confusion[truth_state]:
            confusion[truth_state][estimate_state] += 1
    repaired_indices = {
        index for index, event in enumerate(estimate)
        if str(event.get("provenance", "detected")).startswith("repaired_")
    }
    repaired_matches = sum(estimate_index in repaired_indices for _, estimate_index in pairs)
    return {
        "truth": len(truth),
        "estimated": len(estimate),
        "matched": matches,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "mean_absolute_error_ms": (
            sum(abs(value) for value in errors) / len(errors) if errors else None
        ),
        "tolerance_ms": tolerance * 1000.0,
        "matched_state_accuracy": state_matches / matches if matches else 0.0,
        "state_confusion": confusion,
        "repaired_estimated": len(repaired_indices),
        "repaired_matched": repaired_matches,
        "repaired_precision": repaired_matches / len(repaired_indices) if repaired_indices else None,
    }, pairs


def _chord_parts(label: str) -> tuple[str, str, str]:
    normalized, _ = normalize_chord(label)
    if normalized == "-":
        return normalized, "-", "none"
    root = normalized[0] + (normalized[1] if len(normalized) > 1 and normalized[1] in "#b" else "")
    quality = normalized[len(root):].split("/", 1)[0]
    if quality.startswith("m") and not quality.startswith("maj"):
        family = "minor"
    elif "dim" in quality:
        family = "diminished"
    elif "aug" in quality:
        family = "augmented"
    else:
        family = "major"
    return normalized, root, family


def _chord_score(truth: list[dict[str, object]], estimate: list[dict[str, object]], duration: float) -> dict[str, float]:
    samples = max(1, math.ceil(duration * 20.0))
    exact = root = family = 0
    compared = 0
    for index in range(samples):
        timestamp = min(duration - 1e-6, index / 20.0)
        truth_event = next((item for item in truth if float(item["start"]) <= timestamp < float(item["end"])), None)
        estimate_event = next((item for item in estimate if float(item["start"]) <= timestamp < float(item["end"])), None)
        if truth_event is None:
            continue
        compared += 1
        truth_parts = _chord_parts(str(truth_event["label"]))
        estimate_parts = _chord_parts(str(estimate_event["label"]) if estimate_event else "N")
        exact += truth_parts[0] == estimate_parts[0]
        root += truth_parts[1] == estimate_parts[1]
        family += truth_parts[1:] == estimate_parts[1:]
    return {
        "seconds_compared": compared / 20.0,
        "exact_symbol_accuracy": exact / compared if compared else 0.0,
        "root_accuracy": root / compared if compared else 0.0,
        "root_and_triad_family_accuracy": family / compared if compared else 0.0,
    }


def score_generated_benchmark(truth_path: Path, analysis_path: Path) -> Path:
    truth_path = truth_path.resolve()
    analysis_path = analysis_path.resolve()
    truth = json.loads(truth_path.read_text(encoding="utf-8"))
    analysis_document = json.loads(analysis_path.read_text(encoding="utf-8"))
    analysis = analysis_document["analysis"]
    manifest_path = analysis_path.parent / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    song_folder = Path(manifest["song_folder"])
    estimate_assets = {
        str(item["stem"]): song_folder / Path(str(item["path"]))
        for item in manifest["assets"] if str(item["bank"]) == "A"
    }
    stem_scores = {
        stem: _audio_score(Path(source), estimate_assets[stem])
        for stem, source in truth["truth_stems"].items()
    }

    beat_truth = [(float(value), "beat") for value in truth["beats"]]
    beat_estimate = [(float(value), "beat") for value in analysis["beats"]]
    downbeat_truth = [(float(value), "downbeat") for value in truth["downbeats"]]
    downbeat_estimate = [(float(value), "downbeat") for value in analysis["downbeats"]]

    drum_family = {
        "kick": "kick", "Kick": "kick",
        "snare": "snare", "rim": "snare", "Snare": "snare", "Cross-stick / Rim": "snare",
        "closed_hat": "hihat", "open_hat": "hihat", "Closed HH": "hihat", "Open HH": "hihat",
        "ride": "cymbal", "crash": "cymbal", "Ride": "cymbal", "Crash": "cymbal",
        "high_tom": "tom", "mid_tom": "tom", "floor_tom": "tom",
        "High Tom": "tom", "Mid Tom": "tom", "Floor Tom": "tom",
    }
    def drum_rows(items: list[dict[str, object]]) -> list[dict[str, object]]:
        rows = []
        for item in items:
            lane = str(item["lane"])
            if lane not in drum_family:
                continue
            velocity = int(item.get("velocity", 91))
            rows.append({
                **item,
                "time": max(0.0, float(item["time"])),
                "family": drum_family[lane],
                "state": str(item.get("state", drum_state(velocity))),
                "provenance": str(item.get("provenance", "detected")),
            })
        return rows

    drum_truth_rows = drum_rows(truth["drums"])
    drum_estimate_rows = drum_rows(analysis["drums"])
    drum_truth = [(float(item["time"]), str(item["family"])) for item in drum_truth_rows]
    drum_estimate = [(float(item["time"]), str(item["family"])) for item in drum_estimate_rows]
    drum_analysis_by_family = {
        family: _match_events(
            [item for item in drum_truth if item[1] == family],
            [item for item in drum_estimate if item[1] == family],
            0.05,
        )
        for family in sorted({item[1] for item in drum_truth + drum_estimate})
    }
    drum_truth_velocity_40 = [
        (max(0.0, float(item["time"])), drum_family[str(item["lane"])])
        for item in truth["drums"]
        if str(item["lane"]) in drum_family and int(item.get("velocity", 100)) >= 40
    ]

    jamjar = json.loads((song_folder / str(manifest["jamjar"])).read_text(encoding="utf-8"))
    lane_names = (
        "Kick", "Snare", "Closed HH", "Open HH", "Ride", "Crash",
        "High Tom", "Mid Tom", "Floor Tom", "Cross-stick / Rim",
    )
    accepted_bpm = float(analysis["bpm"])
    grid_estimate_source: list[dict[str, object]] = []
    for beat, pattern in enumerate(jamjar["sections"][0]["beat_patterns"]):
        division = int(pattern["division"])
        for lane_index, value in enumerate(pattern["lanes"]):
            for step, state in enumerate(str(value).lower()[:division]):
                if state in {"g", "x", "a"}:
                    grid_estimate_source.append({
                        "time": (beat + step / division) * 60.0 / accepted_bpm,
                        "lane": lane_names[lane_index],
                        "state": state,
                    })
    quantized = analysis_document.get("quantization", {}).get("A", {}).get("drum_hits", [])
    for event in grid_estimate_source:
        evidence = [
            item for item in quantized
            if str(item["lane"]) == str(event["lane"])
            and abs(float(item["quantized_time"]) - float(event["time"])) <= 0.002
        ]
        if evidence:
            best = min(evidence, key=lambda item: abs(float(item["time"]) - float(event["time"])))
            event["provenance"] = str(best.get("provenance", "detected"))
    grid_truth_rows = drum_rows(truth.get("drum_grid", truth["drums"]))
    grid_estimate_rows = drum_rows(grid_estimate_source)
    drum_analysis_score, _ = _match_drum_events(
        drum_truth_rows, drum_estimate_rows, 0.05
    )
    drum_grid_score, _ = _match_drum_events(
        grid_truth_rows, grid_estimate_rows, 0.05
    )
    drum_grid_by_family = {
        family: _match_drum_events(
            [item for item in grid_truth_rows if item["family"] == family],
            [item for item in grid_estimate_rows if item["family"] == family],
            0.05,
        )[0]
        for family in sorted({
            str(item["family"]) for item in [*grid_truth_rows, *grid_estimate_rows]
        })
    }
    repair_by_reason = {}
    for reason in sorted({
        str(item.get("provenance", "detected")) for item in grid_estimate_rows
        if str(item.get("provenance", "detected")).startswith("repaired_")
    }):
        reason_estimate = [
            item for item in grid_estimate_rows if item.get("provenance") == reason
        ]
        reason_score, _ = _match_drum_events(grid_truth_rows, reason_estimate, 0.05)
        repair_by_reason[reason] = {
            "estimated": reason_score["estimated"],
            "matched": reason_score["matched"],
            "precision": reason_score["precision"],
        }
    fill_regions = truth.get("fill_regions", [])

    def in_fill(event: dict[str, object]) -> bool:
        timestamp = float(event["time"])
        return any(float(region["start"]) <= timestamp < float(region["end"]) for region in fill_regions)

    fill_truth = [event for event in grid_truth_rows if in_fill(event)]
    fill_estimate = [event for event in grid_estimate_rows if in_fill(event)]
    non_fill_truth = [event for event in grid_truth_rows if not in_fill(event)]
    non_fill_estimate = [event for event in grid_estimate_rows if not in_fill(event)]
    drum_fill_score, _ = _match_drum_events(fill_truth, fill_estimate, 0.05)
    drum_non_fill_score, _ = _match_drum_events(non_fill_truth, non_fill_estimate, 0.05)
    drum_fill_by_family = {
        family: _match_drum_events(
            [item for item in fill_truth if item["family"] == family],
            [item for item in fill_estimate if item["family"] == family], 0.05,
        )[0]
        for family in sorted({str(item["family"]) for item in [*fill_truth, *fill_estimate]})
    }
    drum_non_fill_by_family = {
        family: _match_drum_events(
            [item for item in non_fill_truth if item["family"] == family],
            [item for item in non_fill_estimate if item["family"] == family], 0.05,
        )[0]
        for family in sorted({
            str(item["family"]) for item in [*non_fill_truth, *non_fill_estimate]
        })
    }
    bass_truth = [(float(item["start"]), str(item["midi"])) for item in truth["bass"]]
    bass_estimate = [(float(item["start"]), str(item["midi"])) for item in analysis["bass"]]

    detected_bpm = float(analysis_document["detected_bpm"])
    true_bpm = float(truth["bpm"])
    score = {
        "format": "jamtaster-generated-score-v2",
        "truth": str(truth_path),
        "analysis": str(analysis_path),
        "audio_duration_seconds": truth["duration_seconds"],
        "timings": analysis_document["timings"],
        "tempo": {
            "truth_bpm": true_bpm,
            "detected_bpm": detected_bpm,
            "absolute_error_bpm": abs(detected_bpm - true_bpm),
            "absolute_error_percent": abs(detected_bpm - true_bpm) / true_bpm * 100.0,
        },
        "beats": _match_events(beat_truth, beat_estimate, 0.07),
        "downbeats": _match_events(downbeat_truth, downbeat_estimate, 0.10),
        "chords": _chord_score(truth["chords"], analysis["chords"], float(truth["duration_seconds"])),
        "drums": drum_grid_score,
        "drums_analysis": drum_analysis_score,
        "drums_grid": drum_grid_score,
        "drums_grid_fills": drum_fill_score,
        "drums_grid_fills_by_family": drum_fill_by_family,
        "drums_grid_non_fills": drum_non_fill_score,
        "drums_grid_non_fills_by_family": drum_non_fill_by_family,
        "drums_truth_velocity_ge_40": _match_events(
            drum_truth_velocity_40, drum_estimate, 0.05
        ),
        "drums_by_family": drum_grid_by_family,
        "drums_analysis_by_family": drum_analysis_by_family,
        "drum_repairs_by_reason": repair_by_reason,
        "bass_pitch_onsets": _match_events(bass_truth, bass_estimate, 0.10),
        "stems": stem_scores,
        "structure": {
            "scored": False,
            "reason": "the eight-bar fixture has no intro/verse/chorus/bridge form truth",
            "estimated_segments": analysis["structures"],
        },
    }
    output = truth_path.parent / "scores" / f"{analysis_path.parent.name}.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(score, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return output
