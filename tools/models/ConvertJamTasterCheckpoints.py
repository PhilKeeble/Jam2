from __future__ import annotations

import argparse
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT = REPO_ROOT / "experiments" / "jamtaster-native"
MODELS_ROOT = ROOT / "models"
OUTPUTS_ROOT = ROOT / "outputs"
CONVERSION_DEMUCS_ROOT = (
    Path(__file__).resolve().parent / "third_party" / "jamtaster_demucs_onnx"
)
DEMUCS_ONNX_REVISION = "81fa192e6fcc88e35e887f6e6ccce91227f4e6f5"


def _default_component_root() -> Path:
    override = os.environ.get("JAMTASTER_COMPONENT_ROOT", "").strip()
    if override:
        return Path(override)
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    return base / "Jam2" / "components" / "jamtaster" / "1.0.0"


def _component_python(component_root: Path) -> Path:
    candidates = (
        component_root / "runtime" / "Scripts" / "python.exe",
        component_root / "runtime" / "bin" / "python3",
        component_root / "runtime" / "bin" / "python",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    # A partially cleaned Windows venv can retain its packages and pyvenv.cfg
    # while losing the small Scripts/python.exe redirector. Conversion is a
    # developer-only task, so use the recorded base interpreter explicitly and
    # add the component site-packages in main() below.
    configuration = component_root / "runtime" / "pyvenv.cfg"
    if sys.platform == "win32" and configuration.is_file():
        for line in configuration.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator and key.strip().lower() == "home":
                candidate = Path(value.strip()) / "python.exe"
                if candidate.is_file():
                    return candidate
    raise RuntimeError(f"JamTaster development Python is missing under {component_root}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _prepare_ml_imports(component_root: Path) -> None:
    worker_path = str(REPO_ROOT / "app" / "jamtaster" / "worker")
    if worker_path not in sys.path:
        sys.path.insert(0, worker_path)
    os.environ["JAMTASTER_COMPONENT_ROOT"] = str(component_root)
    os.environ["TORCH_HOME"] = str(component_root / "cache" / "torch")


def _checkpoint(component_root: Path, name: str) -> Path:
    return component_root / "cache" / "torch" / "hub" / "checkpoints" / name


def _export_metadata(
    model_path: Path,
    source: dict[str, object],
    parity: dict[str, object],
) -> None:
    metadata = {
        "format": "jamtaster-native-export-v1",
        "model": model_path.name,
        "bytes": model_path.stat().st_size,
        "sha256": _sha256(model_path),
        "source": source,
        "parity": parity,
        "created_at": time.time(),
    }
    model_path.with_suffix(model_path.suffix + ".json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )


def _ort_parity(model_path: Path, input_name: str, sample, expected: list) -> dict[str, float]:
    import numpy as np
    import onnxruntime as ort

    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    actual = session.run(None, {input_name: sample})
    differences = []
    for wanted, received in zip(expected, actual):
        differences.append(np.abs(np.asarray(wanted) - np.asarray(received)))
    return {
        "maximum_absolute_error": max(float(value.max(initial=0.0)) for value in differences),
        "mean_absolute_error": float(np.mean([value.mean() for value in differences])),
    }


def _export_beat_this(component_root: Path) -> Path:
    import numpy as np
    import torch
    from beat_this.inference import load_model

    checkpoint = _checkpoint(component_root, "beat_this-final0.ckpt")
    model = load_model(str(checkpoint), "cpu")

    class Wrapper(torch.nn.Module):
        def __init__(self, wrapped):
            super().__init__()
            self.wrapped = wrapped

        def forward(self, spectrogram):
            result = self.wrapped(spectrogram)
            return result["beat"], result["downbeat"]

    wrapper = Wrapper(model).eval()
    torch.manual_seed(7)
    # Beat This always runs chunks up to its 1,500-frame training length.
    # Tracing a shorter tensor leaves the rotary-position cache specialized to
    # that shorter length even when ONNX axes are marked dynamic.
    sample = torch.randn(1, 1500, 128)
    with torch.inference_mode():
        expected = [value.numpy() for value in wrapper(sample)]
    destination = MODELS_ROOT / "beat_this.onnx"
    MODELS_ROOT.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        wrapper,
        sample,
        destination,
        input_names=["input_spectrogram"],
        output_names=["beat", "downbeat"],
        dynamic_axes={
            "input_spectrogram": {1: "frames"},
            "beat": {1: "frames"},
            "downbeat": {1: "frames"},
        },
        opset_version=17,
        dynamo=False,
    )
    parity = _ort_parity(destination, "input_spectrogram", sample.numpy(), expected)
    _export_metadata(
        destination,
        {"checkpoint": checkpoint.name, "sha256": _sha256(checkpoint), "revision": "final0"},
        parity,
    )
    return destination


def _load_chordmini_model(component_root: Path):
    import argparse as argparse_module
    import importlib.util
    import types
    import torch

    root = component_root / "models" / "chordmini"
    for path in (root, root / "src"):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))
    utility_package = "src.evaluation.utils"
    if utility_package not in sys.modules:
        stub = types.ModuleType(utility_package)
        stub.__package__ = utility_package
        stub.__path__ = [str(root / "src" / "evaluation" / "utils")]
        sys.modules[utility_package] = stub
    script = root / "src" / "evaluation" / "test.py"
    spec = importlib.util.spec_from_file_location("jamtaster_native_chordmini", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the pinned ChordMini inference module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    checkpoint = root / "checkpoints" / "btc_model_best.pth"
    config = module.HParams.load(str(root / "config" / "ChordMini.yaml"))
    arguments = argparse_module.Namespace(
        model_type="BTC", checkpoint=str(checkpoint), model_file=None, seed=42,
        verbose=False, smooth_predictions=False, smooth_logits=False, kernel_size=9,
        use_gaussian=False, use_overlap=False, overlap_ratio=None,
        vote_aggregation="logit", min_segment_duration=0.25,
    )
    model, _, _ = module.load_model(
        str(checkpoint), "BTC", config, torch.device("cpu"), arguments
    )
    return model.eval(), checkpoint


def _export_chordmini(component_root: Path) -> Path:
    import torch

    model, checkpoint = _load_chordmini_model(component_root)
    torch.manual_seed(11)
    sample = torch.randn(1, 108, 144)
    with torch.inference_mode():
        expected = [model(sample).numpy()]
    destination = MODELS_ROOT / "chordmini_btc.onnx"
    MODELS_ROOT.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        model,
        sample,
        destination,
        input_names=["cqt"],
        output_names=["chord_logits"],
        opset_version=17,
        dynamo=False,
    )
    parity = _ort_parity(destination, "cqt", sample.numpy(), expected)
    _export_metadata(
        destination,
        {"checkpoint": "chordmini/checkpoints/" + checkpoint.name, "sha256": _sha256(checkpoint)},
        parity,
    )
    return destination


def _export_adtof(component_root: Path) -> Path:
    import torch
    from adtof_pytorch import calculate_n_bins, create_frame_rnn_model, get_default_weights_path
    from adtof_pytorch import load_pytorch_weights

    model = create_frame_rnn_model(calculate_n_bins())
    checkpoint = Path(get_default_weights_path())
    model = load_pytorch_weights(model, str(checkpoint), strict=False).eval()
    torch.manual_seed(13)
    sample = torch.randn(1, 256, model.n_bins, 1)
    with torch.inference_mode():
        expected = [model(sample).numpy()]
    destination = MODELS_ROOT / "adtof.onnx"
    MODELS_ROOT.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        model,
        sample,
        destination,
        input_names=["log_filterbank"],
        output_names=["drum_probabilities"],
        dynamic_axes={"log_filterbank": {1: "frames"}, "drum_probabilities": {1: "frames"}},
        opset_version=17,
        dynamo=False,
    )
    parity = _ort_parity(destination, "log_filterbank", sample.numpy(), expected)
    _export_metadata(
        destination,
        {"checkpoint": checkpoint.name, "sha256": _sha256(checkpoint), "redistribution": "unresolved"},
        parity,
    )
    return destination


def _export_demucs_ft(component_root: Path) -> list[Path]:
    import torch
    import torch.nn.functional as functional

    demucs_source = CONVERSION_DEMUCS_ROOT
    if str(demucs_source) not in sys.path:
        sys.path.insert(0, str(demucs_source))
    from demucs.htdemucs import standalone_magnitude, standalone_spec
    from demucs.pretrained import get_model

    bag = get_model("htdemucs_ft")
    if not hasattr(bag, "models") or len(bag.models) != 4:
        raise RuntimeError("pinned htdemucs_ft did not resolve to four ensemble members")
    destinations = []
    for index, model in enumerate(bag.models):
        model = model.cpu().eval()
        training_length = int(model.segment * model.samplerate)
        waveform = torch.zeros(1, 2, training_length)
        magnitude = standalone_magnitude(standalone_spec(waveform))
        destination = MODELS_ROOT / f"htdemucs_ft_{index}.onnx"
        MODELS_ROOT.mkdir(parents=True, exist_ok=True)
        torch.onnx.export(
            model,
            (waveform, magnitude),
            destination,
            input_names=["mix", "magnitude"],
            output_names=["frequency", "time"],
            opset_version=17,
            dynamo=False,
        )
        signature = ["f7e0c4bc", "d12395a8", "92cfc3b6", "04573f0d"][index]
        checkpoint = next((component_root / "cache" / "torch" / "hub" / "checkpoints").glob(f"{signature}-*.th"))
        _export_metadata(
            destination,
            {
                "checkpoint": checkpoint.name,
                "sha256": _sha256(checkpoint),
                "signature": signature,
                "ensemble_role": ["drums", "bass", "other", "vocals"][index],
                "demucs_onnx_revision": DEMUCS_ONNX_REVISION,
            },
            {
                "status": "validated-native-audio-parity",
                "reference": "experiments/jamtaster-native/RESULTS.md",
            },
        )
        destinations.append(destination)
        print(f"Exported Demucs FT member {index + 1}/4: {destination}", flush=True)
    return destinations


def export_models(args: argparse.Namespace) -> None:
    component_root = Path(args.component_root).resolve()
    _prepare_ml_imports(component_root)
    selected = [args.model] if args.model != "all" else [
        "beat-this", "basic-pitch", "chordmini", "adtof", "demucs-ft"
    ]
    for name in selected:
        started = time.perf_counter()
        if name == "beat-this":
            paths = [_export_beat_this(component_root)]
        elif name == "basic-pitch":
            candidates = list((component_root / "runtime").glob(
                "**/basic_pitch/saved_models/icassp_2022/nmp.onnx"
            ))
            if not candidates:
                raise RuntimeError("official Basic Pitch ONNX model is missing")
            MODELS_ROOT.mkdir(parents=True, exist_ok=True)
            destination = MODELS_ROOT / "basic_pitch.onnx"
            shutil.copy2(candidates[0], destination)
            _export_metadata(
                destination,
                {
                    "source": "basic_pitch/saved_models/icassp_2022/nmp.onnx",
                    "license": "Apache-2.0",
                },
                {"maximum_absolute_error": 0.0, "mean_absolute_error": 0.0},
            )
            paths = [destination]
        elif name == "chordmini":
            paths = [_export_chordmini(component_root)]
        elif name == "adtof":
            paths = [_export_adtof(component_root)]
        elif name == "demucs-ft":
            paths = _export_demucs_ft(component_root)
        else:
            raise RuntimeError(f"unsupported export model: {name}")
        total = sum(path.stat().st_size for path in paths)
        print(
            f"{name}: {len(paths)} artifact(s), {total / (1024 * 1024):.1f} MiB, "
            f"{time.perf_counter() - started:.2f}s",
            flush=True,
        )


def reference(args: argparse.Namespace) -> None:
    component_root = Path(args.component_root).resolve()
    _prepare_ml_imports(component_root)
    if args.stage == "demucs":
        import numpy as np
        import soundfile as sound_file
        import torch
        import torchaudio.functional as audio_functional
        from demucs_infer.api import Separator

        threads = max(1, int(args.threads))
        torch.set_num_threads(threads)
        signal, sample_rate = sound_file.read(
            args.input, dtype="float32", always_2d=True
        )
        source_frames = signal.shape[0]
        value = torch.from_numpy(np.ascontiguousarray(signal.T))
        separator = Separator(model="htdemucs_ft", device="cpu", progress=True)
        if sample_rate != separator.samplerate:
            value = audio_functional.resample(value, sample_rate, separator.samplerate)
        if value.shape[0] == 1 and separator.audio_channels > 1:
            value = value.repeat(separator.audio_channels, 1).contiguous()
        started = time.perf_counter()
        _, sources = separator.separate_tensor(value.clone(), separator.samplerate)
        inference_seconds = time.perf_counter() - started
        output_root = Path(args.output).resolve()
        output_root.mkdir(parents=True, exist_ok=True)
        stem_paths: dict[str, str] = {}
        for stem in ("drums", "bass", "other", "vocals"):
            audio = sources[stem].detach().float().cpu()
            if audio.ndim == 3:
                audio = audio[0]
            if audio.ndim == 2:
                audio = audio.mean(dim=0)
            if sample_rate != separator.samplerate:
                audio = audio_functional.resample(
                    audio, separator.samplerate, sample_rate
                )
            samples = audio.numpy()
            if len(samples) < source_frames:
                samples = np.pad(samples, (0, source_frames - len(samples)))
            samples = samples[:source_frames]
            path = output_root / f"{stem}.wav"
            sound_file.write(path, samples, sample_rate, subtype="PCM_16")
            stem_paths[stem] = str(path)
        report = {
            "format": "jamtaster-python-demucs-v1",
            "input": str(Path(args.input).resolve()),
            "model": "htdemucs_ft",
            "provider": "CPU",
            "threads": threads,
            "sample_rate": sample_rate,
            "frames": source_frames,
            "stems": stem_paths,
            "timings": {"inference": inference_seconds},
        }
        report_path = output_root / "reference.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(report_path)
        return

    output_root = Path(args.output).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    if args.stage == "basic-pitch":
        from basic_pitch import ICASSP_2022_MODEL_PATH
        from basic_pitch.inference import Model, predict

        minimum_frequency = 440.0 * 2 ** ((args.min_midi - 69) / 12)
        maximum_frequency = 440.0 * 2 ** ((args.max_midi - 69) / 12)
        started = time.perf_counter()
        _, _, events = predict(
            args.input,
            Model(ICASSP_2022_MODEL_PATH),
            minimum_frequency=minimum_frequency,
            maximum_frequency=maximum_frequency,
        )
        duration = time.perf_counter() - started
        notes = [
            {
                "start": float(start), "end": float(end), "midi": int(midi),
                "velocity": int(round(127 * confidence)),
                "confidence": float(confidence),
            }
            for start, end, midi, confidence, _ in events
            if args.min_midi <= int(midi) <= args.max_midi
        ]
        report = {
            "format": "jamtaster-python-basic-pitch-v1",
            "input": str(Path(args.input).resolve()),
            "model": str(ICASSP_2022_MODEL_PATH),
            "provider": "CPU",
            "midi_range": [args.min_midi, args.max_midi],
            "notes": notes,
            "timings": {"complete_analysis": duration},
        }
    elif args.stage == "chords":
        from jamtaster.models import analyze_chords

        started = time.perf_counter()
        segments, _ = analyze_chords(Path(args.input), output_root, "cpu")
        duration = time.perf_counter() - started
        report = {
            "format": "jamtaster-python-chords-v1",
            "input": str(Path(args.input).resolve()),
            "model": "ChordMini BTC",
            "provider": "CPU",
            "chords": [
                {"start": item.start, "end": item.end, "label": item.label}
                for item in segments
            ],
            "timings": {"complete_analysis": duration},
        }
    elif args.stage == "drums":
        import numpy as np
        from adtof_pytorch import transcribe_to_midi
        from adtof_pytorch.post_processing import LABELS_5, PeakPicker

        thresholds = [float(value.strip()) for value in args.thresholds.split(",")]
        if len(thresholds) != 5:
            raise RuntimeError("--thresholds requires five comma-separated values")
        started = time.perf_counter()
        activations = transcribe_to_midi(
            args.input, output_root / "unused.mid", return_activations=True, device="cpu"
        )
        peaks = PeakPicker(thresholds=thresholds, fps=100).pick(
            activations, labels=LABELS_5
        )[0]
        duration = time.perf_counter() - started
        lanes = {35: "Kick", 38: "Snare", 47: "Mid Tom", 42: "Closed HH", 49: "Crash"}
        hits = []
        for class_index, midi in enumerate(LABELS_5):
            for event_time in peaks[int(midi)]:
                frame = max(0, min(activations.shape[1] - 1, round(event_time * 100.0)))
                hits.append({
                    "time": float(event_time), "midi": int(midi), "lane": lanes[int(midi)],
                    "confidence": float(activations[0, frame, class_index]),
                })
        report = {
            "format": "jamtaster-python-drums-v1",
            "input": str(Path(args.input).resolve()),
            "model": "ADTOF Frame RNN",
            "provider": "CPU",
            "thresholds": thresholds,
            "hits": sorted(hits, key=lambda item: (item["time"], item["midi"])),
            "activation_frames": int(np.asarray(activations).shape[1]),
            "timings": {"complete_analysis": duration},
        }
    else:
        report = None
    if report is not None:
        report_path = output_root / "reference.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(report_path)
        return

    import numpy as np
    import soundfile as sound_file
    from beat_this.inference import Audio2Frames
    from beat_this.model.postprocessor import Postprocessor

    signal, sample_rate = sound_file.read(args.input, dtype="float32", always_2d=False)
    analyzer = Audio2Frames(str(_checkpoint(component_root, "beat_this-final0.ckpt")), device="cpu")
    started = time.perf_counter()
    beat_logits, downbeat_logits = analyzer(signal, sample_rate)
    inference_seconds = time.perf_counter() - started
    beats, downbeats = Postprocessor(type="minimal")(beat_logits, downbeat_logits)
    beat_path = output_root / "beat_logits.f32"
    downbeat_path = output_root / "downbeat_logits.f32"
    with beat_path.open("wb") as destination:
        array("f", np.asarray(beat_logits, dtype=np.float32)).tofile(destination)
    with downbeat_path.open("wb") as destination:
        array("f", np.asarray(downbeat_logits, dtype=np.float32)).tofile(destination)
    report = {
        "format": "jamtaster-python-beats-v1",
        "input": str(Path(args.input).resolve()),
        "model": "beat_this-final0.ckpt",
        "provider": "CPU",
        "beats": np.asarray(beats).tolist(),
        "downbeats": np.asarray(downbeats).tolist(),
        "beat_logits": str(beat_path),
        "downbeat_logits": str(downbeat_path),
        "logit_frames": len(beat_logits),
        "timings": {"inference_and_preprocessing": inference_seconds},
    }
    report_path = output_root / "reference.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report_path)


def _match_events(reference_values: list[float], native_values: list[float], tolerance: float) -> dict[str, float]:
    remaining = set(range(len(native_values)))
    differences = []
    for wanted in reference_values:
        candidates = [(abs(native_values[index] - wanted), index) for index in remaining]
        if not candidates:
            continue
        difference, index = min(candidates)
        if difference <= tolerance:
            remaining.remove(index)
            differences.append(difference)
    matches = len(differences)
    precision = matches / len(native_values) if native_values else 1.0 if not reference_values else 0.0
    recall = matches / len(reference_values) if reference_values else 1.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {
        "reference": len(reference_values),
        "native": len(native_values),
        "matches": matches,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "mean_error_ms": 1000.0 * sum(differences) / matches if matches else math.nan,
        "maximum_error_ms": 1000.0 * max(differences) if differences else math.nan,
    }


def _read_floats(path: str, count: int) -> list[float]:
    values = array("f")
    with Path(path).open("rb") as source:
        values.fromfile(source, count)
    if len(values) != count:
        raise RuntimeError(f"tensor dump is truncated: {path}")
    return list(values)


def _match_labeled_events(
    reference_values: list[dict[str, object]],
    native_values: list[dict[str, object]],
    time_key: str,
    label_key: str,
    tolerance: float,
) -> dict[str, float]:
    remaining = set(range(len(native_values)))
    differences = []
    for wanted in reference_values:
        candidates = [
            (abs(float(native_values[index][time_key]) - float(wanted[time_key])), index)
            for index in remaining
            if native_values[index][label_key] == wanted[label_key]
        ]
        if not candidates:
            continue
        difference, index = min(candidates)
        if difference <= tolerance:
            remaining.remove(index)
            differences.append(difference)
    matches = len(differences)
    precision = matches / len(native_values) if native_values else 1.0 if not reference_values else 0.0
    recall = matches / len(reference_values) if reference_values else 1.0
    return {
        "reference": len(reference_values), "native": len(native_values), "matches": matches,
        "precision": precision, "recall": recall,
        "f1": 2 * precision * recall / (precision + recall) if precision + recall else 0.0,
        "mean_error_ms": 1000.0 * sum(differences) / matches if matches else math.nan,
        "maximum_error_ms": 1000.0 * max(differences) if differences else math.nan,
    }


def _chord_frame_agreement(
    reference_values: list[dict[str, object]], native_values: list[dict[str, object]]
) -> dict[str, float]:
    end = min(
        max((float(item["end"]) for item in reference_values), default=0.0),
        max((float(item["end"]) for item in native_values), default=0.0),
    )
    frames = int(end * 100)
    matches = 0
    compared = 0
    for frame in range(frames):
        time_value = (frame + 0.5) / 100.0
        wanted = next((item["label"] for item in reference_values
                       if float(item["start"]) <= time_value < float(item["end"])), None)
        actual = next((item["label"] for item in native_values
                       if float(item["start"]) <= time_value < float(item["end"])), None)
        if wanted is not None and actual is not None:
            compared += 1
            matches += wanted == actual
    return {
        "duration_seconds": end, "compared_frames": compared, "matching_frames": matches,
        "accuracy": matches / compared if compared else math.nan,
    }


def compare(args: argparse.Namespace) -> None:
    reference_report = json.loads(Path(args.reference).read_text(encoding="utf-8"))
    native_report = json.loads(Path(args.native).read_text(encoding="utf-8"))
    tolerance = args.tolerance_ms / 1000.0
    report: dict[str, object] = {
        "format": "jamtaster-native-parity-v1",
        "reference": str(Path(args.reference).resolve()),
        "native": str(Path(args.native).resolve()),
        "tolerance_ms": args.tolerance_ms,
    }
    if "beats" in reference_report and "beats" in native_report:
        report["beats"] = _match_events(reference_report["beats"], native_report["beats"], tolerance)
        report["downbeats"] = _match_events(
            reference_report["downbeats"], native_report["downbeats"], tolerance
        )
    elif "notes" in reference_report and "notes" in native_report:
        report["note_onsets"] = _match_labeled_events(
            reference_report["notes"], native_report["notes"], "start", "midi", tolerance
        )
    elif "hits" in reference_report and "hits" in native_report:
        report["drum_hits"] = _match_labeled_events(
            reference_report["hits"], native_report["hits"], "time", "midi", tolerance
        )
    elif "chords" in reference_report and "chords" in native_report:
        report["chord_frames"] = _chord_frame_agreement(
            reference_report["chords"], native_report["chords"]
        )
    else:
        raise RuntimeError("reference and native reports do not contain matching event types")
    count = min(reference_report.get("logit_frames", 0), native_report.get("logit_frames", 0))
    if count:
        for name in ("beat_logits", "downbeat_logits"):
            wanted = _read_floats(reference_report[name], count)
            actual = _read_floats(native_report[name], count)
            differences = [abs(left - right) for left, right in zip(wanted, actual)]
            report[name] = {
                "frames": count,
                "mean_absolute_error": sum(differences) / count,
                "maximum_absolute_error": max(differences),
            }
    output = json.dumps(report, indent=2, allow_nan=True) + "\n"
    if args.output:
        Path(args.output).write_text(output, encoding="utf-8")
    print(output, end="")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ConvertJamTasterCheckpoints.py",
        description="One-off JamTaster checkpoint conversion and native parity utility.",
    )
    parser.add_argument("--component-root", default=str(_default_component_root()))
    parser.add_argument("--inside-component", action="store_true", help=argparse.SUPPRESS)
    commands = parser.add_subparsers(dest="command", required=True)

    export_parser = commands.add_parser("export", help="convert pinned checkpoints to ONNX once")
    export_parser.add_argument(
        "model", choices=("beat-this", "basic-pitch", "chordmini", "adtof", "demucs-ft", "all")
    )

    reference_parser = commands.add_parser("reference", help="write Python reference tensors and events")
    reference_parser.add_argument(
        "--stage", choices=("beat", "basic-pitch", "chords", "drums", "demucs"),
        default="beat",
    )
    reference_parser.add_argument("--input", required=True)
    reference_parser.add_argument("--output", required=True)
    reference_parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 1))
    reference_parser.add_argument("--min-midi", type=int, default=28)
    reference_parser.add_argument("--max-midi", type=int, default=64)
    reference_parser.add_argument(
        "--thresholds", default="0.22,0.24,0.32,0.22,0.30"
    )

    compare_parser = commands.add_parser("compare", help="compare native results to a Python reference")
    compare_parser.add_argument("--reference", required=True)
    compare_parser.add_argument("--native", required=True)
    compare_parser.add_argument("--tolerance-ms", type=float, default=30.0)
    compare_parser.add_argument("--output")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.command in {"export", "reference"} and not args.inside_component:
        component_root = Path(args.component_root).resolve()
        python = _component_python(component_root)
        environment = dict(os.environ)
        python_path = [str(REPO_ROOT / "app" / "jamtaster" / "worker")]
        if sys.platform == "win32":
            site_packages = component_root / "runtime" / "Lib" / "site-packages"
        else:
            version = f"python{sys.version_info.major}.{sys.version_info.minor}"
            site_packages = component_root / "runtime" / "lib" / version / "site-packages"
        if site_packages.is_dir():
            python_path.append(str(site_packages))
        if environment.get("PYTHONPATH"):
            python_path.append(environment["PYTHONPATH"])
        environment["PYTHONPATH"] = os.pathsep.join(python_path)
        completed = subprocess.run(
            [str(python), str(Path(__file__).resolve()), "--inside-component", *sys.argv[1:]],
            env=environment,
            check=False,
        )
        return completed.returncode
    if args.command == "export":
        export_models(args)
    elif args.command == "reference":
        reference(args)
    elif args.command == "compare":
        compare(args)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Cancelled", file=sys.stderr)
        raise SystemExit(130)
    except Exception as error:
        print(f"JamTaster native tool failed: {error}", file=sys.stderr)
        raise SystemExit(1)
