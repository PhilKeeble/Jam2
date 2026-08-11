from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
from typing import Any

from .events import EventSink
from .models import configure_local_caches, resolve_device, separate_stems, track_beats
from .paths import PROTOCOL_VERSION, project_analysis_root, portable_slug
from .timing import estimate_bpm, infer_meter
from .wav import inspect_loopback_wav, sha256_file


DEFAULTS: dict[str, Any] = {
    "device": "auto",
    "sections": "auto",
    "bpm": None,
    "meter": "auto",
    "no_arrangement_loop": False,
    "no_time_stretch": False,
    "demucs_model": "htdemucs_ft",
    "beat_model": "final0",
    "drum_thresholds": "0.12,0.16,0.12,0.05,0.20",
    "drum_candidate_scale": 0.30,
    "drum_division": 4,
    "drum_ghost_energy_ratio": 0.50,
    "drum_accent_energy_ratio": 0.85,
    "drum_repair_min_repeats": 3,
    "drum_repair_neighborhood_bars": 8,
    "no_drum_repair": False,
    "bass_min_midi": 28,
    "bass_max_midi": 60,
    "drift_warning_ms": 100.0,
    "local_drift_warning_ms": 45.0,
    "local_warp_threshold_ms": 45.0,
    "force": False,
}


def run_request(request: dict[str, Any], emit: EventSink) -> dict[str, Any]:
    if int(request.get("protocol", 0)) != PROTOCOL_VERSION:
        raise ValueError(
            f"unsupported JamTaster protocol {request.get('protocol')}; expected {PROTOCOL_VERSION}"
        )
    action = str(request.get("action", "")).strip()
    input_value = str(request.get("input_path", "")).strip()
    project_value = str(request.get("project_root", "")).strip()
    if not input_value:
        raise ValueError("input_path is required")
    if not project_value:
        raise ValueError("project_root is required")
    input_path = Path(input_value)
    project_root = Path(project_value)
    fallback: dict[str, Any] | None = None
    failure_detail = ""
    try:
        return _dispatch_request(action, input_path, project_root, request, emit)
    except RuntimeError as exc:
        requested = str(request.get("device", "auto"))
        if action in {"analyze_all", "convert_song"}:
            requested = str(request.get("options", {}).get("device", "auto"))
        if requested != "auto" or not _accelerator_failure(exc):
            raise
        fallback = dict(request)
        if action in {"analyze_all", "convert_song"}:
            fallback["options"] = dict(request.get("options", {}))
            fallback["options"]["device"] = "cpu"
        else:
            fallback["device"] = "cpu"
        failure_detail = str(exc)
    emit({
        "type": "progress",
        "stage": "accelerator_fallback",
        "message": "Graphics acceleration failed; retrying safely on CPU",
        "detail": failure_detail,
    })
    assert fallback is not None
    return _dispatch_request(action, input_path, project_root, fallback, emit)


def _dispatch_request(
    action: str,
    input_path: Path,
    project_root: Path,
    request: dict[str, Any],
    emit: EventSink,
) -> dict[str, Any]:
    if action == "detect_bpm":
        return detect_bpm(input_path, project_root, request, emit)
    if action == "split_stems":
        return split_stems(input_path, project_root, request, emit)
    if action in {"analyze_all", "convert_song"}:
        from .pipeline import taste
        values = dict(DEFAULTS)
        values.update(request.get("options", {}))
        values.update({
            "input": str(input_path),
            "name": str(request.get("display_name", input_path.stem)).strip(),
            "project_root": str(project_root),
            "event_sink": emit,
        })
        output = taste(argparse.Namespace(**values))
        return {
            "format": "jamtaster-job-result-v1",
            "action": action,
            "input_path": str(input_path.resolve()),
            "project_root": str(project_root.resolve()),
            "converted_song": str(output),
        }
    raise ValueError(f"unsupported JamTaster action: {action}")


def _accelerator_failure(error: RuntimeError) -> bool:
    detail = str(error).lower()
    return any(marker in detail for marker in (
        "cuda", "cudnn", "cublas", "gpu", "device-side", "out of memory",
    ))


def detect_bpm(
    input_path: Path,
    project_root: Path,
    request: dict[str, Any],
    emit: EventSink,
) -> dict[str, Any]:
    configure_local_caches()
    started = time.perf_counter()
    info = inspect_loopback_wav(input_path)
    source_hash = sha256_file(info.path)
    root = _source_root(project_root, source_hash)
    result_path = root / "tempo.json"
    if result_path.is_file() and not bool(request.get("force", False)):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        emit({"type": "progress", "stage": "tempo", "message": "Using saved tempo analysis", "cached": True})
        return result
    emit({"type": "progress", "stage": "tempo", "message": "Detecting tempo and downbeats"})
    device = resolve_device(str(request.get("device", "auto")))
    beats, downbeats = track_beats(info.path, device, str(request.get("beat_model", "final0")))
    result = {
        "format": "jamtaster-tempo-v1",
        "action": "detect_bpm",
        "input_path": str(info.path),
        "source_sha256": source_hash,
        "bpm": estimate_bpm(beats),
        "project_bpm": round(estimate_bpm(beats)),
        "beats_per_bar": infer_meter(beats, downbeats),
        "beats": beats,
        "downbeats": downbeats,
        "elapsed_seconds": time.perf_counter() - started,
    }
    _write_json(result_path, result)
    _update_index(project_root, source_hash, info.path, {"tempo": str(result_path)})
    emit({"type": "progress", "stage": "tempo", "message": "Tempo analysis complete", "percent": 100})
    return result


def split_stems(
    input_path: Path,
    project_root: Path,
    request: dict[str, Any],
    emit: EventSink,
) -> dict[str, Any]:
    configure_local_caches()
    started = time.perf_counter()
    info = inspect_loopback_wav(input_path)
    source_hash = sha256_file(info.path)
    root = _source_root(project_root, source_hash)
    stems_root = root / "stems"
    expected = {name: stems_root / f"{name}.wav" for name in ("drums", "bass", "other", "vocals")}
    result_path = root / "stems.json"
    if all(path.is_file() for path in expected.values()) and result_path.is_file() and not bool(request.get("force", False)):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        emit({"type": "progress", "stage": "separation", "message": "Using saved stem analysis", "cached": True})
        return result
    device = resolve_device(str(request.get("device", "auto")))
    working_root = project_analysis_root(project_root) / ".working"
    working_root.mkdir(parents=True, exist_ok=True)
    emit({"type": "progress", "stage": "separation", "message": "Splitting drums, bass, vocals and other"})
    with tempfile.TemporaryDirectory(prefix="stems-", dir=working_root) as temporary:
        generated = separate_stems(
            info.path,
            info,
            Path(temporary),
            device,
            str(request.get("demucs_model", "htdemucs_ft")),
        )
        stems_root.mkdir(parents=True, exist_ok=True)
        for name, destination in expected.items():
            partial = destination.with_suffix(".wav.partial")
            shutil.copy2(generated[name], partial)
            os.replace(partial, destination)
    result = {
        "format": "jamtaster-stems-v1",
        "action": "split_stems",
        "input_path": str(info.path),
        "source_sha256": source_hash,
        "device": device,
        "stems": {name: str(path) for name, path in expected.items()},
        "elapsed_seconds": time.perf_counter() - started,
    }
    _write_json(result_path, result)
    _update_index(project_root, source_hash, info.path, {"stems": str(result_path)})
    emit({"type": "progress", "stage": "separation", "message": "Stem separation complete", "percent": 100})
    return result


def _source_root(project_root: Path, source_hash: str) -> Path:
    root = project_analysis_root(project_root) / "sources" / source_hash
    root.mkdir(parents=True, exist_ok=True)
    return root


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _update_index(
    project_root: Path,
    source_hash: str,
    input_path: Path,
    results: dict[str, str],
) -> None:
    path = project_analysis_root(project_root) / "index.json"
    index: dict[str, Any] = {"format": "jamtaster-analysis-index-v1", "sources": {}}
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict) and isinstance(loaded.get("sources"), dict):
                index = loaded
        except (OSError, json.JSONDecodeError):
            pass
    source = dict(index["sources"].get(source_hash, {}))
    source.update({
        "input_path": str(input_path.resolve()),
        "source_sha256": source_hash,
        "updated_at": time.time(),
    })
    source_results = dict(source.get("results", {}))
    source_results.update(results)
    source["results"] = source_results
    index["sources"][source_hash] = source
    _write_json(path, index)
