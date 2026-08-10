from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import time
from typing import Callable

from .models import (
    analyze_bass, analyze_chords, analyze_chroma_chords, analyze_drums,
    prepare_analysis_only_chord_audio,
)
from .paths import CACHE_ROOT, DATASETS_ROOT, REPORTS_ROOT
from .postprocess import classify_cymbals, contextualize_bass, contextualize_chords
from .timing import normalize_chord, repair_drum_hits, shape_drum_dynamics
from .types import DrumHit, NoteEvent, TimedLabel, jsonable


@dataclass(frozen=True)
class ReferenceItem:
    dataset: str
    identifier: str
    audio: Path
    truth: Path
    split: str


def stable_split(identifier: str) -> str:
    """Keep one fifth of each corpus untouched while rules are tuned."""
    return "heldout" if hashlib.sha256(identifier.encode("utf-8")).digest()[0] % 5 == 0 else "dev"


def _guitarset_items() -> list[ReferenceItem]:
    root = DATASETS_ROOT / "guitarset" / "extracted" / "GuitarSet"
    annotation = root / "annotation"
    audio = root / "audio" / "audio_mic"
    return [
        ReferenceItem("guitarset", path.stem, audio / f"{path.stem}_mic.wav", path, stable_split(path.stem))
        for path in sorted(annotation.glob("*.jams"))
        if (audio / f"{path.stem}_mic.wav").is_file()
    ]


def _filobass_items() -> list[ReferenceItem]:
    root = DATASETS_ROOT / "filobass" / "extracted" / "FiloBass ISMIR Publication"
    audio = root / "audio_bass_stems"
    midi = root / "midi_fully_aligned"
    return [
        ReferenceItem("filobass", path.stem, audio / f"{path.stem}.mp3", path, stable_split(path.stem))
        for path in sorted(midi.glob("*.mid"))
        if (audio / f"{path.stem}.mp3").is_file()
    ]


def _mdb_items() -> list[ReferenceItem]:
    root = DATASETS_ROOT / "mdb-drums" / "MDB Drums"
    annotation = root / "annotations" / "subclass"
    audio = root / "audio" / "drum_only"
    result = []
    for path in sorted(annotation.glob("*_subclass.txt")):
        identifier = path.name.removesuffix("_subclass.txt")
        source = audio / f"{identifier}_Drum.wav"
        if source.is_file():
            result.append(ReferenceItem("mdb-drums", identifier, source, path, stable_split(identifier)))
    return result


ADAPTERS: dict[str, Callable[[], list[ReferenceItem]]] = {
    "guitarset": _guitarset_items,
    "filobass": _filobass_items,
    "mdb-drums": _mdb_items,
}


def inventory() -> dict[str, object]:
    guitar = _guitarset_items()
    bass = _filobass_items()
    drums = _mdb_items()
    groove_root = DATASETS_ROOT / "groove-v1.0.0"
    harmonix_root = DATASETS_ROOT / "harmonixset"
    return {
        "format": "jamtaster-dataset-inventory-v1",
        "root": str(DATASETS_ROOT),
        "corpora": {
            "guitarset": {
                "usable_pairs": len(guitar), "tasks": ["chords", "beats", "guitar_notes"],
                "dev": sum(item.split == "dev" for item in guitar),
                "heldout": sum(item.split == "heldout" for item in guitar),
            },
            "filobass": {
                "usable_pairs": len(bass), "tasks": ["bass_notes", "bass_timing"],
                "dev": sum(item.split == "dev" for item in bass),
                "heldout": sum(item.split == "heldout" for item in bass),
            },
            "mdb-drums": {
                "usable_pairs": len(drums), "tasks": ["drum_hits", "drum_classes", "beats"],
                "dev": sum(item.split == "dev" for item in drums),
                "heldout": sum(item.split == "heldout" for item in drums),
            },
            "groove-midi": {
                "audio_files": len(list(groove_root.rglob("*.wav"))) if groove_root.exists() else 0,
                "midi_files": len(list(groove_root.rglob("*.mid"))) if groove_root.exists() else 0,
                "tasks": ["drum_hits", "dynamics", "microtiming"],
            },
            "harmonixset": {
                "annotation_files": len(list(harmonix_root.rglob("*.txt"))) if harmonix_root.exists() else 0,
                "tasks": ["chords", "beats", "sections"],
                "status": "annotations only; matching audio is not bundled",
            },
        },
        "policy": {
            "split": "sha256(identifier), deterministic 80/20 development/heldout",
            "heldout_use": "report only; never tune rules from heldout tracks",
            "stem_only_corpora": "not counted as transcription accuracy references",
        },
    }


def write_inventory() -> Path:
    DATASETS_ROOT.mkdir(parents=True, exist_ok=True)
    output = DATASETS_ROOT / "inventory.json"
    output.write_text(json.dumps(inventory(), indent=2) + "\n", encoding="utf-8")
    return output


def _chord_truth(path: Path) -> tuple[list[TimedLabel], list[float], float]:
    document = json.loads(path.read_text(encoding="utf-8"))
    annotations = document["annotations"]
    chord_layers = [item for item in annotations if item["namespace"] == "chord"]
    # The first layer is GuitarSet's simplified vocabulary; the second includes
    # inversions and omissions that Jam2 cannot represent.
    layer = chord_layers[0]
    chords = [TimedLabel(float(row["time"]), float(row["time"] + row["duration"]), str(row["value"])) for row in layer["data"]]
    beat_layer = next(item for item in annotations if item["namespace"] == "beat_position")
    beats = [float(row["time"]) for row in beat_layer["data"]]
    return chords, beats, float(document["file_metadata"]["duration"])


def _chord_accuracy(truth: list[TimedLabel], estimate: list[TimedLabel], duration: float) -> dict[str, float]:
    pitch_class = {
        "C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3,
        "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
        "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11,
    }

    def parts(label: str) -> tuple[int | None, str]:
        normalized, _ = normalize_chord(label)
        if normalized == "-":
            return None, "N"
        root = normalized[:2] if len(normalized) > 1 and normalized[1] in "#b" else normalized[:1]
        quality = normalized[len(root):].split("/", 1)[0]
        return pitch_class[root], quality

    exact = root = compared = 0
    for index in range(max(1, int(duration * 20.0))):
        timestamp = min(duration - 1e-6, index / 20.0)
        wanted = next((item.label for item in truth if item.start <= timestamp < item.end), "N")
        got = next((item.label for item in estimate if item.start <= timestamp < item.end), "N")
        wanted_parts = parts(wanted)
        got_parts = parts(got)
        if wanted_parts[0] is None:
            continue
        compared += 1
        exact += wanted_parts == got_parts
        root += wanted_parts[0] == got_parts[0]
    return {
        "exact": exact / compared if compared else 0.0,
        "root": root / compared if compared else 0.0,
        "seconds": compared / 20.0,
    }


def _midi_notes(path: Path) -> list[NoteEvent]:
    import pretty_midi
    midi = pretty_midi.PrettyMIDI(str(path))
    return sorted(
        [NoteEvent(note.start, note.end, note.pitch, note.velocity) for instrument in midi.instruments for note in instrument.notes],
        key=lambda note: (note.start, note.midi),
    )


def _note_score(truth: list[NoteEvent], estimate: list[NoteEvent]) -> dict[str, float]:
    import numpy as np
    import mir_eval
    truth_intervals = np.asarray([[note.start, note.end] for note in truth], dtype=float).reshape((-1, 2))
    estimate_intervals = np.asarray([[note.start, note.end] for note in estimate], dtype=float).reshape((-1, 2))
    truth_pitch = np.asarray([440.0 * 2 ** ((note.midi - 69) / 12) for note in truth])
    estimate_pitch = np.asarray([440.0 * 2 ** ((note.midi - 69) / 12) for note in estimate])
    precision, recall, f1, overlap = mir_eval.transcription.precision_recall_f1_overlap(
        truth_intervals, truth_pitch, estimate_intervals, estimate_pitch, onset_tolerance=0.05
    )
    return {"precision": precision, "recall": recall, "f1": f1, "overlap": overlap}


DRUM_TRUTH = {
    "KD": "kick", "SD": "snare", "SDB": "snare", "SDD": "snare", "SDF": "snare",
    "SDG": "snare", "SDNS": "snare", "SST": "snare",
    "CHH": "hihat", "PHH": "hihat", "OHH": "hihat",
    "HFT": "tom", "LFT": "tom", "MHT": "tom", "HIT": "tom", "TMB": "tom",
    "CRC": "crash", "CHC": "crash", "SPC": "crash", "RDB": "ride", "RDC": "ride",
}
DRUM_ESTIMATE = {
    "Kick": "kick", "Snare": "snare", "Cross-stick / Rim": "snare", "Closed HH": "hihat",
    "Open HH": "hihat", "Mid Tom": "tom", "Crash": "crash", "Ride": "ride",
}


def _drum_truth(path: Path) -> list[tuple[float, str]]:
    result = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in DRUM_TRUTH:
            result.append((float(fields[0]), DRUM_TRUTH[fields[1]]))
    return result


def _event_score(truth: list[tuple[float, str]], estimate: list[tuple[float, str]], tolerance: float = 0.05) -> dict[str, float]:
    matched_truth: set[int] = set()
    matched = 0
    for time_value, label in estimate:
        candidates = [(abs(time_value - wanted_time), index) for index, (wanted_time, wanted_label) in enumerate(truth) if index not in matched_truth and wanted_label == label and abs(time_value - wanted_time) <= tolerance]
        if candidates:
            _, index = min(candidates)
            matched_truth.add(index)
            matched += 1
    precision = matched / len(estimate) if estimate else 0.0
    recall = matched / len(truth) if truth else 0.0
    return {"precision": precision, "recall": recall, "f1": 2 * precision * recall / (precision + recall) if precision + recall else 0.0, "truth": len(truth), "estimate": len(estimate)}


def _event_scores_by_class(
    truth: list[tuple[float, str]], estimate: list[tuple[float, str]]
) -> dict[str, dict[str, float]]:
    return {
        label: _event_score(
            [event for event in truth if event[1] == label],
            [event for event in estimate if event[1] == label],
        )
        for label in sorted({event[1] for event in [*truth, *estimate]})
    }


def _mean_metrics(rows: list[dict[str, object]], field: str) -> dict[str, float]:
    keys = rows[0][field].keys() if rows else []
    return {key: statistics.mean(float(row[field][key]) for row in rows) for key in keys if isinstance(rows[0][field][key], (int, float))}


def run_benchmark(dataset: str, split: str, limit: int, device: str) -> Path:
    if dataset not in ADAPTERS:
        raise ValueError(f"unknown reference dataset: {dataset}")
    items = [item for item in ADAPTERS[dataset]() if split == "all" or item.split == split][:limit]
    if not items:
        raise ValueError(f"no {dataset} items available for split {split}")
    rows: list[dict[str, object]] = []
    root = CACHE_ROOT / "dataset-benchmarks"
    root.mkdir(parents=True, exist_ok=True)
    for item in items:
        started = time.perf_counter()
        print(f"Reference {dataset}/{item.identifier} ({item.split})...")
        with tempfile.TemporaryDirectory(prefix="reference-", dir=root) as folder:
            workspace = Path(folder)
            if dataset == "guitarset":
                truth, beats, duration = _chord_truth(item.truth)
                raw_workspace = workspace / "raw"
                refined_workspace = workspace / "refined"
                raw_workspace.mkdir()
                refined_workspace.mkdir()
                raw, _ = analyze_chords(item.audio, raw_workspace, device)
                refined_audio = workspace / "harmonic-midrange.wav"
                prepare_analysis_only_chord_audio(item.audio, refined_audio)
                refined, _ = analyze_chords(refined_audio, refined_workspace, device)
                _, chroma = analyze_chroma_chords(item.audio, beats)
                harmonic_audio = workspace / "hpss-evidence.wav"
                prepare_analysis_only_chord_audio(item.audio, harmonic_audio, harmonic=True)
                _, refined_chroma = analyze_chroma_chords(harmonic_audio, beats)
                post, diagnostics = contextualize_chords(
                    raw, [chroma], [], beats, [], duration
                )
                hpss_post, _ = contextualize_chords(
                    raw, [chroma, refined_chroma], [], beats, [], duration
                )
                row = {
                    "dataset": dataset, "identifier": item.identifier, "split": item.split,
                    "raw": _chord_accuracy(truth, raw, duration),
                    "preprocessed": _chord_accuracy(truth, refined, duration),
                    "postprocessed": _chord_accuracy(truth, post, duration),
                    "context_variants": {
                        "raw_chroma_only": _chord_accuracy(truth, post, duration),
                        "raw_plus_hpss_chroma": _chord_accuracy(truth, hpss_post, duration),
                    },
                    "corrections": len(diagnostics.get("corrections", [])) + int(diagnostics.get("decoder_changes", 0)),
                }
            elif dataset == "filobass":
                truth = _midi_notes(item.truth)
                raw = analyze_bass(item.audio, 28, 60)
                post, diagnostics = contextualize_bass(raw, [])
                variants = {
                    f"min_{round(threshold * 1000)}ms": _note_score(
                        truth, contextualize_bass(raw, [], threshold)[0]
                    )
                    for threshold in (0.035, 0.055, 0.075, 0.100)
                }
                row = {
                    "dataset": dataset, "identifier": item.identifier, "split": item.split,
                    "raw": _note_score(truth, raw), "postprocessed": _note_score(truth, post),
                    "variants": variants,
                    "corrections": len(diagnostics),
                }
            else:
                truth = _drum_truth(item.truth)
                beat_path = item.truth.parents[1] / "beats" / f"{item.identifier}_MIX.beats"
                beat_rows = [line.split() for line in beat_path.read_text(encoding="utf-8").splitlines() if line.split()]
                beats = [float(fields[0]) for fields in beat_rows]
                downbeats = [float(fields[0]) for fields in beat_rows if len(fields) > 1 and fields[1] == "1"]
                bpm = 60.0 / statistics.median(
                    current - previous for previous, current in zip(beats, beats[1:])
                    if current > previous
                )
                raw, candidates, _ = analyze_drums(item.audio, workspace, device, "0.12,0.16,0.12,0.05,0.20", 0.30, 0.50, 0.85)
                repaired, _ = repair_drum_hits(raw, candidates, beats, bpm, 4, 4, {"Kick": .12, "Snare": .16, "Mid Tom": .12, "Closed HH": .05, "Crash": .20}, 3, 8, True, False)
                shaped, _ = shape_drum_dynamics(repaired, beats, bpm, 4, 4, downbeats)
                context_only, context_only_diagnostics = classify_cymbals(
                    raw, beats, downbeats, []
                )
                post, diagnostics = classify_cymbals(shaped, beats, downbeats, [])
                repair_variants = {}
                for minimum_repeats in (4, 5):
                    variant_repaired, _ = repair_drum_hits(
                        raw, candidates, beats, bpm, 4, 4,
                        {"Kick": .12, "Snare": .16, "Mid Tom": .12, "Closed HH": .05, "Crash": .20},
                        minimum_repeats, 8, True, False,
                    )
                    variant_shaped, _ = shape_drum_dynamics(
                        variant_repaired, beats, bpm, 4, 4, downbeats
                    )
                    variant_post, _ = classify_cymbals(
                        variant_shaped, beats, downbeats, []
                    )
                    variant_events = [
                        (hit.time, DRUM_ESTIMATE[hit.lane])
                        for hit in variant_post if hit.lane in DRUM_ESTIMATE
                    ]
                    repair_variants[f"min_repeats_{minimum_repeats}"] = _event_score(
                        truth, variant_events
                    )
                raw_events = [(hit.time, DRUM_ESTIMATE[hit.lane]) for hit in raw if hit.lane in DRUM_ESTIMATE]
                repaired_events = [(hit.time, DRUM_ESTIMATE[hit.lane]) for hit in shaped if hit.lane in DRUM_ESTIMATE]
                context_only_events = [(hit.time, DRUM_ESTIMATE[hit.lane]) for hit in context_only if hit.lane in DRUM_ESTIMATE]
                post_events = [(hit.time, DRUM_ESTIMATE[hit.lane]) for hit in post if hit.lane in DRUM_ESTIMATE]
                repair_provenance = {}
                for provenance in ("repaired_transient", "repaired_repetition", "repaired_fill_cluster"):
                    events = [
                        (hit.time, DRUM_ESTIMATE[hit.lane]) for hit in shaped
                        if hit.provenance == provenance and hit.lane in DRUM_ESTIMATE
                    ]
                    repair_provenance[provenance] = _event_score(truth, events)
                row = {
                    "dataset": dataset, "identifier": item.identifier, "split": item.split,
                    "raw": _event_score(truth, raw_events),
                    "repaired": _event_score(truth, repaired_events),
                    "context_only": _event_score(truth, context_only_events),
                    "postprocessed": _event_score(truth, post_events),
                    "raw_by_class": _event_scores_by_class(truth, raw_events),
                    "postprocessed_by_class": _event_scores_by_class(truth, post_events),
                    "cymbal_corrections": len(diagnostics),
                    "context_only_cymbal_corrections": len(context_only_diagnostics),
                    "repair_variants": repair_variants,
                    "repair_provenance": repair_provenance,
                }
        row["seconds"] = time.perf_counter() - started
        rows.append(row)
    report = {
        "format": "jamtaster-reference-benchmark-v1", "dataset": dataset, "requested_split": split,
        "items": rows, "raw_mean": _mean_metrics(rows, "raw"),
        "preprocessed_mean": _mean_metrics(rows, "preprocessed") if "preprocessed" in rows[0] else None,
        "repaired_mean": _mean_metrics(rows, "repaired") if "repaired" in rows[0] else None,
        "context_only_mean": _mean_metrics(rows, "context_only") if "context_only" in rows[0] else None,
        "postprocessed_mean": _mean_metrics(rows, "postprocessed"),
        "elapsed_seconds": sum(float(row["seconds"]) for row in rows),
    }
    report_root = REPORTS_ROOT / "datasets"
    report_root.mkdir(parents=True, exist_ok=True)
    output = report_root / f"{dataset}-{split}-{int(time.time())}.json"
    output.write_text(json.dumps(jsonable(report), indent=2) + "\n", encoding="utf-8")
    return output
