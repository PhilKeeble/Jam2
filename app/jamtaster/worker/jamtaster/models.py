from __future__ import annotations

from array import array
import argparse
import gc
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
import types
import urllib.request
import wave
import zipfile

from .paths import CACHE_ROOT, MODELS_ROOT
from .types import DrumHit, NoteEvent, TimedLabel, WavInfo


CHORDMINI_REVISION = "aa6e3a8d7b017f082fd2aaff9329d5c26af49c03"
ADTOF_REVISION = "85c192e78f716ea0b111cc8a5ee4a8f6a3a4f8a9"


class ModelError(RuntimeError):
    pass


def _model_progress(message: str, percent: int) -> None:
    print(json.dumps({
        "format": "jamtaster-install-progress-v1",
        "message": message,
        "percent": percent,
    }, separators=(",", ":")), flush=True)


def configure_local_caches() -> None:
    locations = {
        "TORCH_HOME": CACHE_ROOT / "torch",
        "XDG_CACHE_HOME": CACHE_ROOT,
        "NUMBA_CACHE_DIR": CACHE_ROOT / "numba",
        "PIP_CACHE_DIR": CACHE_ROOT / "pip",
    }
    for name, path in locations.items():
        path.mkdir(parents=True, exist_ok=True)
        os.environ[name] = str(path)
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")


def resolve_device(requested: str) -> str:
    if requested != "auto":
        if requested.startswith("cuda"):
            try:
                import torch
            except ImportError as exc:
                raise ModelError("the JamTaster runtime is incomplete; use Repair in Jam2 Settings") from exc
            if not torch.cuda.is_available():
                raise ModelError(f"requested {requested}, but CUDA is unavailable")
        return requested
    try:
        import torch
        return "cuda" if torch.cuda.is_available() else "cpu"
    except ImportError:
        return "cpu"


def _safe_extract_zip(archive: Path, destination: Path) -> None:
    destination_resolved = destination.resolve()
    with zipfile.ZipFile(archive) as bundle:
        members = bundle.infolist()
        for member in members:
            target = (destination / member.filename).resolve()
            if destination_resolved not in target.parents and target != destination_resolved:
                raise ModelError(f"unsafe path in upstream archive: {member.filename}")
        bundle.extractall(destination)


def _fetch_github_snapshot(owner: str, repo: str, revision: str, destination: Path) -> None:
    marker = destination / ".jamtaster-revision"
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == revision:
        return
    if destination.exists():
        raise ModelError(
            f"{destination} exists but is not the pinned {revision} snapshot; remove it and rerun models fetch"
        )
    downloads = CACHE_ROOT / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / f"{repo}-{revision}.zip"
    if not archive.is_file():
        url = f"https://codeload.github.com/{owner}/{repo}/zip/{revision}"
        print(f"Downloading {repo} source {revision[:12]}...")
        urllib.request.urlretrieve(url, archive)
    unpack = downloads / f"unpack-{repo}-{revision}"
    if unpack.exists():
        import shutil
        shutil.rmtree(unpack)
    unpack.mkdir()
    _safe_extract_zip(archive, unpack)
    roots = [entry for entry in unpack.iterdir() if entry.is_dir()]
    if len(roots) != 1:
        raise ModelError(f"unexpected {repo} source archive layout")
    roots[0].replace(destination)
    marker.write_text(revision + "\n", encoding="utf-8")
    import shutil
    shutil.rmtree(unpack)


def fetch_models(device: str) -> None:
    configure_local_caches()
    MODELS_ROOT.mkdir(parents=True, exist_ok=True)
    _model_progress("Downloading pinned chord model", 63)
    _fetch_github_snapshot(
        "ptnghia-j", "ChordMini", CHORDMINI_REVISION, MODELS_ROOT / "chordmini"
    )
    chord_checkpoint = MODELS_ROOT / "chordmini" / "checkpoints" / "btc_model_best.pth"
    if not chord_checkpoint.is_file() or chord_checkpoint.stat().st_size <= 1024 * 1024:
        raise ModelError("the pinned ChordMini archive did not contain its full BTC checkpoint")

    _model_progress("Downloading pinned chord and beat models", 76)
    try:
        from beat_this.inference import File2Beats
        File2Beats(checkpoint_path="final0", device=device, dbn=False)
    except Exception as exc:
        raise ModelError(f"could not fetch Beat This weights: {exc}") from exc

    _model_progress("Downloading pinned Demucs stem model", 84)
    try:
        from demucs_infer.api import Separator
        separator = Separator(model="htdemucs_ft", device=device)
        del separator
    except Exception as exc:
        raise ModelError(f"could not fetch Demucs weights: {exc}") from exc

    _model_progress("Validating local model weights", 92)
    _model_progress("Pinned model installation complete", 96)


def _write_tensor_wav(tensor: object, sample_rate: int, path: Path) -> None:
    import numpy as np
    value = tensor.detach().float().cpu()
    if value.ndim == 3:
        value = value[0]
    if value.ndim == 2:
        value = value.mean(dim=0)
    if value.ndim != 1:
        raise ModelError(f"unexpected separated stem shape: {tuple(value.shape)}")
    pcm = (value.clamp(-1.0, 1.0).numpy() * 32767.0).round().astype(np.int16)
    with wave.open(str(path), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(sample_rate)
        target.writeframes(pcm.tobytes())


def separate_stems(input_path: Path, info: WavInfo, workspace: Path, device: str, model: str) -> dict[str, Path]:
    try:
        import torch
        import torchaudio.functional as audio_functional
        import soundfile as sf
        from demucs_infer.api import Separator
    except ImportError as exc:
        raise ModelError("Demucs is missing; use Repair in Jam2 Settings") from exc
    print(f"Separating drums, bass, other and vocals with {model} on {device}...")
    try:
        separator = Separator(model=model, device=device, progress=True)
        samples, source_rate = sf.read(str(input_path), dtype="float32", always_2d=False)
        value = torch.from_numpy(samples).unsqueeze(0)
        if source_rate != separator.samplerate:
            value = audio_functional.resample(value, source_rate, separator.samplerate)
        if value.shape[0] == 1 and separator.audio_channels > 1:
            # demucs-infer expands mono with a shared-memory view and then
            # normalizes it in place. Materialize independent channels at our
            # adapter boundary so mono 44.1 kHz files remain supported.
            value = value.repeat(separator.audio_channels, 1).contiguous()
        _, sources = separator.separate_tensor(value.clone(), separator.samplerate)
    except Exception as exc:
        raise ModelError(f"Demucs separation failed: {exc}") from exc
    result: dict[str, Path] = {}
    for stem in ("drums", "bass", "other", "vocals"):
        if stem not in sources:
            raise ModelError(f"Demucs model did not return required {stem!r} stem")
        value = sources[stem]
        if info.sample_rate != 44100:
            value = audio_functional.resample(value, 44100, info.sample_rate)
        path = workspace / f"full-{stem}.wav"
        _write_tensor_wav(value, info.sample_rate, path)
        result[stem] = path
    del sources, separator
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()
    return result


def track_beats(input_path: Path, device: str, checkpoint: str) -> tuple[list[float], list[float]]:
    try:
        from beat_this.inference import File2Beats
    except ImportError as exc:
        raise ModelError("Beat This is missing; use Repair in Jam2 Settings") from exc
    print(f"Tracking beats and downbeats with Beat This {checkpoint}...")
    try:
        tracker = File2Beats(checkpoint_path=checkpoint, device=device, dbn=False)
        beats, downbeats = tracker(str(input_path))
        return [float(value) for value in beats], [float(value) for value in downbeats]
    except Exception as exc:
        raise ModelError(f"Beat This analysis failed: {exc}") from exc


def mix_pcm16(left: Path, right: Path, destination: Path) -> None:
    with wave.open(str(left), "rb") as first, wave.open(str(right), "rb") as second:
        parameters = (first.getnchannels(), first.getsampwidth(), first.getframerate())
        if parameters != (second.getnchannels(), second.getsampwidth(), second.getframerate()):
            raise ModelError("cannot mix stems with mismatched WAV formats")
        with wave.open(str(destination), "wb") as target:
            target.setparams(first.getparams())
            while True:
                a = first.readframes(65536)
                b = second.readframes(65536)
                if not a or not b:
                    break
                av = array("h"); bv = array("h")
                av.frombytes(a); bv.frombytes(b)
                mixed = array("h", (max(-32768, min(32767, x + y)) for x, y in zip(av, bv)))
                target.writeframesraw(mixed.tobytes())
            target.writeframes(b"")


def prepare_chord_sources(
    other_path: Path, bass_path: Path, workspace: Path
) -> dict[str, Path]:
    """Create the measured-best chord source without altering exported stems."""
    raw_path = workspace / "chords-raw-other-plus-bass.wav"
    mix_pcm16(other_path, bass_path, raw_path)
    return {"raw": raw_path}


def prepare_analysis_only_chord_audio(
    source_path: Path, destination: Path, harmonic: bool = False
) -> None:
    """Suppress percussive/noisy bands in an analyser copy of harmonic audio."""
    try:
        import librosa
        import numpy as np
        import soundfile as sf
        from scipy.signal import butter, sosfiltfilt
    except ImportError as exc:
        raise ModelError("harmonic preprocessing support is missing; use Repair in Jam2 Settings") from exc
    other, sample_rate = sf.read(str(source_path), dtype="float32", always_2d=False)
    if other.ndim != 1:
        other = other.mean(axis=1)
    # Keep attacks for the primary model: GuitarSet measurements show that an
    # HPSS-only ChordMini input discards useful strum evidence. HPSS remains an
    # optional secondary chroma view.
    source = librosa.effects.harmonic(other, margin=2.0) if harmonic else other
    high = min(7200.0, sample_rate * 0.45)
    sos = butter(2, (65.0, high), btype="bandpass", fs=sample_rate, output="sos")
    focused = sosfiltfilt(sos, source).astype(np.float32, copy=False)
    peak = float(np.max(np.abs(focused))) if len(focused) else 0.0
    if peak > 0.98:
        focused *= 0.98 / peak
    destination.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(destination), focused, sample_rate, subtype="PCM_16")


def analyze_chords(audio_path: Path, workspace: Path, device: str) -> tuple[list[TimedLabel], list[dict[str, object]]]:
    root = MODELS_ROOT / "chordmini"
    script = root / "src" / "evaluation" / "test.py"
    checkpoint = root / "checkpoints" / "btc_model_best.pth"
    config = root / "config" / "ChordMini.yaml"
    if not script.is_file() or not checkpoint.is_file():
        raise ModelError("ChordMini source/checkpoint is missing; run JamTaster.py models fetch")
    print("Recognising chords with ChordMini BTC in the JamTaster worker...", flush=True)
    stage_started = time.perf_counter()
    # ChordMini is fetched as pinned source, not installed as an importable
    # distribution. Load its inference module locally so a frozen JamTaster
    # worker never tries to spawn sys.executable as a second Python process.
    root_text = str(root)
    source_text = str(root / "src")
    if root_text not in sys.path:
        sys.path.insert(0, root_text)
    if source_text not in sys.path:
        sys.path.insert(0, source_text)
    # Importing a submodule normally executes ChordMini's
    # src.evaluation.utils.__init__, which eagerly imports optional plotting
    # and offline quality-report modules (Matplotlib and Seaborn). Inference
    # only needs common.py and inference.py, so register the package path
    # without executing that plotting-only initializer.
    utility_package = "src.evaluation.utils"
    if utility_package not in sys.modules:
        utility_stub = types.ModuleType(utility_package)
        utility_stub.__package__ = utility_package
        utility_stub.__path__ = [str(root / "src" / "evaluation" / "utils")]
        sys.modules[utility_package] = utility_stub
    try:
        spec = importlib.util.spec_from_file_location("jamtaster_chordmini_inference", script)
        if spec is None or spec.loader is None:
            raise ModelError("could not load ChordMini inference module")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        print(
            f"ChordMini source loaded in {time.perf_counter() - stage_started:.3f}s",
            flush=True,
        )
        import numpy as np
        import torch

        requested_device = torch.device(device)
        arguments = argparse.Namespace(
            model_type="BTC",
            checkpoint=str(checkpoint),
            model_file=None,
            seed=42,
            verbose=False,
            smooth_predictions=True,
            smooth_logits=True,
            kernel_size=9,
            use_gaussian=True,
            use_overlap=True,
            overlap_ratio=None,
            vote_aggregation="logit",
            min_segment_duration=0.25,
        )
        module.set_random_seed(arguments.seed, include_python_random=True)
        chord_config = module.HParams.load(str(config))
        stage_started = time.perf_counter()
        model, _, _ = module.load_model(
            str(checkpoint), arguments.model_type, chord_config,
            requested_device, arguments,
        )
        print(
            f"ChordMini model loaded in {time.perf_counter() - stage_started:.3f}s",
            flush=True,
        )
        mean, std = module._extract_norm_stats(str(checkpoint))
        index_to_chord = module.idx2voca_chord()
        model.idx_to_chord = index_to_chord
        model.eval()
        sequence_length = module._resolve_seq_len(
            chord_config, model, str(checkpoint)
        )
        stage_started = time.perf_counter()
        features, frame_duration, song_duration = (
            module._extract_song_features_root_compatible(
                str(audio_path), chord_config
            )
        )
        print(
            f"ChordMini CQT prepared in {time.perf_counter() - stage_started:.3f}s "
            f"({len(features)} frames)",
            flush=True,
        )
        stage_started = time.perf_counter()
        predictions = module.predict_sliding_windows(
            model=model,
            feature_matrix=features,
            mean=mean,
            std=std,
            seq_len=sequence_length,
            batch_size=16,
            model_type="BTC",
            n_classes=170,
            vote_aggregation="logit",
            use_overlap=True,
            overlap_ratio=None,
            smooth_logits=True,
            smooth_predictions=True,
            kernel_size=9,
            use_gaussian=True,
        )
        print(
            f"ChordMini inference completed in {time.perf_counter() - stage_started:.3f}s",
            flush=True,
        )
        maximum_frames = (
            int(np.floor(song_duration / frame_duration))
            if frame_duration > 0 else len(predictions)
        )
        if maximum_frames > 0:
            predictions = predictions[:maximum_frames]
        lines = module._prediction_segments(
            predictions, frame_duration, index_to_chord, 0.25
        )
        segments: list[TimedLabel] = []
        raw: list[dict[str, object]] = []
        for line in lines:
            fields = line.split(maxsplit=2)
            if len(fields) != 3:
                continue
            start, end, label = float(fields[0]), float(fields[1]), fields[2]
            segments.append(TimedLabel(start, end, label))
            raw.append({"start": start, "end": end, "label": label})
        del model
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()
        return segments, raw
    except ModelError:
        raise
    except Exception as exc:
        raise ModelError(f"ChordMini analysis failed: {exc}") from exc


def analyze_chroma_chords(
    audio_path: Path, beats: list[float]
) -> tuple[list[TimedLabel], list[dict[str, object]]]:
    """Beat-synchronous local fallback for sparse/no-chord model output."""
    import librosa
    import numpy as np

    audio, sample_rate = librosa.load(str(audio_path), sr=None, mono=True)
    hop = 512
    chroma = librosa.feature.chroma_cqt(y=audio, sr=sample_rate, hop_length=hop)
    frame_times = librosa.frames_to_time(
        np.arange(chroma.shape[1]), sr=sample_rate, hop_length=hop
    )
    qualities = {
        "": (0, 4, 7), "m": (0, 3, 7), "7": (0, 4, 7, 10),
        "maj7": (0, 4, 7, 11), "m7": (0, 3, 7, 10),
        "dim": (0, 3, 6), "m7b5": (0, 3, 6, 10),
    }
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    boundaries = [*beats]
    if beats:
        final_interval = beats[-1] - beats[-2] if len(beats) > 1 else 0.5
        boundaries.append(min(len(audio) / sample_rate, beats[-1] + final_interval))
    result: list[TimedLabel] = []
    raw: list[dict[str, object]] = []
    for start, end in zip(boundaries, boundaries[1:]):
        indices = np.flatnonzero((frame_times >= start) & (frame_times < end))
        if not len(indices):
            continue
        profile = np.median(chroma[:, indices], axis=1)
        total = float(np.sum(profile))
        if total <= 1e-8:
            label, confidence = "N", 0.0
        else:
            profile = profile / total
            candidates: list[tuple[float, int, str]] = []
            for root in range(12):
                for quality, intervals in qualities.items():
                    inside = sum(float(profile[(root + interval) % 12]) for interval in intervals)
                    outside = 1.0 - inside
                    # Reward explained energy while mildly preferring the
                    # smaller triad unless a seventh is actually audible.
                    score = inside - 0.45 * outside - 0.012 * (len(intervals) - 3)
                    candidates.append((score, root, quality))
            candidates.sort(reverse=True)
            score, root, quality = candidates[0]
            confidence = score - candidates[1][0]
            label = names[root] + quality
        raw.append({
            "start": start, "end": end, "label": label,
            "confidence_margin": confidence,
            "profile": [round(float(value), 8) for value in profile] if total > 1e-8 else [0.0] * 12,
            "candidates": [
                {"label": names[root] + quality, "score": round(float(score), 8)}
                for score, root, quality in candidates[:5]
            ] if total > 1e-8 else [],
        })
        if result and result[-1].label == label:
            previous = result[-1]
            result[-1] = TimedLabel(previous.start, end, label, confidence)
        else:
            result.append(TimedLabel(start, end, label, confidence))
    return result, raw


def _drum_energy_ratios(
    audio_path: Path,
    core: list[DrumHit],
    candidates: list[DrumHit],
    ghost_ratio: float,
    accent_ratio: float,
) -> tuple[list[DrumHit], list[DrumHit]]:
    import numpy as np
    import soundfile as sf
    from scipy.signal import butter, sosfiltfilt

    audio, sample_rate = sf.read(str(audio_path), dtype="float32", always_2d=False)
    if audio.ndim != 1:
        audio = audio.mean(axis=1)
    bands = {
        "Kick": (30.0, 180.0),
        "Snare": (180.0, 7000.0),
        "Mid Tom": (55.0, 700.0),
        "Cross-stick / Rim": (180.0, 2500.0),
        "Closed HH": (3500.0, min(18000.0, sample_rate * 0.45)),
        "Crash": (2500.0, min(18000.0, sample_rate * 0.45)),
    }
    by_lane: dict[str, list[DrumHit]] = {}
    for hit in [*core, *candidates]:
        by_lane.setdefault(hit.lane, []).append(hit)
    energy: dict[tuple[str, float], float] = {}
    for lane, hits in by_lane.items():
        low, high = bands[lane]
        if high <= low:
            filtered = audio
        else:
            sos = butter(3, (low, high), btype="bandpass", fs=sample_rate, output="sos")
            filtered = sosfiltfilt(sos, audio).astype(np.float32, copy=False)
        for hit in hits:
            first = max(0, round((hit.time - 0.005) * sample_rate))
            last = min(len(filtered), round((hit.time + 0.060) * sample_rate))
            window = filtered[first:last]
            value = float(np.sqrt(np.mean(np.square(window)))) if len(window) else 0.0
            energy[(lane, hit.time)] = value

    references: dict[str, float] = {}
    for lane in by_lane:
        values = [energy[(hit.lane, hit.time)] for hit in core if hit.lane == lane]
        if not values:
            values = [energy[(hit.lane, hit.time)] for hit in candidates if hit.lane == lane]
        references[lane] = max(1e-9, float(np.percentile(values, 90.0))) if values else 1.0

    def enrich(hit: DrumHit) -> DrumHit:
        ratio = energy[(hit.lane, hit.time)] / references[hit.lane]
        lane_ghost = ghost_ratio
        lane_accent = accent_ratio
        # Kick and hi-hat amplitudes occupy quite different ranges. These
        # bounded lane calibrations retain the two public controls while
        # avoiding a single boundary that turns ordinary kicks into accents.
        if hit.lane == "Kick":
            lane_accent = max(accent_ratio, 0.85)
        elif hit.lane == "Mid Tom":
            lane_ghost = min(ghost_ratio, 0.20)
        elif hit.lane == "Closed HH":
            lane_ghost = min(ghost_ratio, 0.40)
            lane_accent = min(accent_ratio, 0.70)
        velocity = 38 if ratio <= lane_ghost else 122 if ratio >= lane_accent else 91
        return DrumHit(
            hit.time, hit.lane, velocity, hit.confidence, round(ratio, 6), hit.provenance
        )

    return [enrich(hit) for hit in core], [enrich(hit) for hit in candidates]


def analyze_drums(
    audio_path: Path,
    workspace: Path,
    device: str,
    thresholds: str,
    candidate_scale: float,
    ghost_ratio: float,
    accent_ratio: float,
) -> tuple[list[DrumHit], list[DrumHit], dict[str, object]]:
    try:
        from adtof_pytorch import transcribe_to_midi
        from adtof_pytorch.post_processing import LABELS_5, PeakPicker
    except ImportError as exc:
        raise ModelError("ADTOF is missing; use Repair in Jam2 Settings") from exc
    print("Transcribing core drum hits with ADTOF-pytorch...")
    try:
        threshold_values = [float(value.strip()) for value in thresholds.split(",")]
        if len(threshold_values) != 5 or any(not 0.0 <= value <= 1.0 for value in threshold_values):
            raise ValueError("expected five comma-separated thresholds between 0 and 1")
        activations = transcribe_to_midi(
            str(audio_path), str(workspace / "unused.mid"), return_activations=True, device=device
        )
    except Exception as exc:
        raise ModelError(f"ADTOF drum analysis failed: {exc}") from exc
    mapping = {
        35: "Kick", 38: "Snare", 47: "Mid Tom", 42: "Closed HH", 49: "Crash",
    }
    import numpy as np

    def picked_hits(picker_thresholds: list[float], provenance: str) -> list[DrumHit]:
        peaks = PeakPicker(thresholds=picker_thresholds, fps=100).pick(
            activations, labels=LABELS_5
        )[0]
        result: list[DrumHit] = []
        for class_index, pitch in enumerate(LABELS_5):
            lane = mapping[int(pitch)]
            for event_time in peaks[int(pitch)]:
                frame = max(0, min(activations.shape[1] - 1, round(event_time * 100.0)))
                confidence = float(activations[0, frame, class_index])
                result.append(DrumHit(float(event_time), lane, 91, confidence, None, provenance))
        return sorted(result, key=lambda hit: (hit.time, hit.lane))

    core = picked_hits(threshold_values, "detected")
    initial_counts = {
        lane: sum(hit.lane == lane for hit in core)
        for lane in mapping.values()
    }
    tom_times = [hit.time for hit in core if hit.lane == "Mid Tom"]
    activation_duration = activations.shape[1] / 100.0
    tom_span = max(tom_times) - min(tom_times) if len(tom_times) >= 2 else 0.0
    rim_mode = (
        initial_counts["Mid Tom"] >= 16
        and initial_counts["Mid Tom"] > initial_counts["Snare"] * 2
        and tom_span >= activation_duration * 0.60
    )
    if rim_mode:
        core = [
            DrumHit(
                hit.time,
                "Cross-stick / Rim" if hit.lane == "Mid Tom" else hit.lane,
                hit.velocity, hit.confidence, hit.energy_ratio, hit.provenance,
            )
            for hit in core
        ]
    permissive = picked_hits([value * candidate_scale for value in threshold_values], "candidate")
    if rim_mode:
        permissive = [
            DrumHit(
                hit.time,
                "Cross-stick / Rim" if hit.lane == "Mid Tom" else hit.lane,
                hit.velocity, hit.confidence, hit.energy_ratio, hit.provenance,
            )
            for hit in permissive
        ]
    candidates = [
        hit for hit in permissive
        if not any(core_hit.lane == hit.lane and abs(core_hit.time - hit.time) <= 0.03 for core_hit in core)
    ]
    # ADTOF can contain useful tom evidence without producing a local peak,
    # especially through fast fills. Couple its activation with an independent
    # broadband transient before offering the event to conservative repair.
    transient_count = 0
    try:
        import librosa
        import soundfile as sf

        audio, sample_rate = sf.read(str(audio_path), dtype="float32", always_2d=False)
        if audio.ndim != 1:
            audio = audio.mean(axis=1)
        hop_length = 128
        envelope = librosa.onset.onset_strength(
            y=audio, sr=sample_rate, hop_length=hop_length, aggregate=np.median
        )
        onset_times = librosa.onset.onset_detect(
            onset_envelope=envelope, sr=sample_rate, hop_length=hop_length,
            units="time", delta=0.05, wait=1, pre_max=1, post_max=1,
            pre_avg=3, post_avg=3,
        )
        tom_floor = max(0.01, threshold_values[2] * candidate_scale * 0.30)
        core_toms = [hit for hit in core if hit.lane == "Mid Tom"]
        for onset_time in (() if rim_mode else onset_times):
            frame = round(float(onset_time) * 100.0)
            first = max(0, frame - 3)
            last = min(activations.shape[1], frame + 4)
            if first >= last:
                continue
            local_activations = activations[0, first:last, :]
            local = local_activations[:, 2]
            local_index = int(np.argmax(local))
            confidence = float(local[local_index])
            event_time = (first + local_index) / 100.0
            lane_peaks = np.max(local_activations, axis=0)
            if (
                confidence < tom_floor
                or float(lane_peaks[3]) >= 0.25
                or float(lane_peaks[4]) >= 0.015
                or any(abs(hit.time - event_time) <= 0.05 for hit in core_toms)
            ):
                continue
            existing_index = next((
                index for index, hit in enumerate(candidates)
                if hit.lane == "Mid Tom" and abs(hit.time - event_time) <= 0.05
            ), None)
            if existing_index is None:
                candidates.append(DrumHit(
                    event_time, "Mid Tom", 91, confidence, None,
                    "candidate_transient",
                ))
            else:
                existing = candidates[existing_index]
                candidates[existing_index] = DrumHit(
                    existing.time, existing.lane, existing.velocity,
                    max(existing.confidence or 0.0, confidence), existing.energy_ratio,
                    "candidate_transient",
                )
            transient_count += 1
    except Exception as exc:
        print(f"Warning: transient tom candidates unavailable: {exc}")
    candidates.sort(key=lambda hit: (hit.time, hit.lane))
    enriched_core, enriched_candidates = _drum_energy_ratios(
        audio_path, core, candidates, ghost_ratio, accent_ratio
    )
    return enriched_core, enriched_candidates, {
        "initial_core_counts": initial_counts,
        "rim_mode": rim_mode,
        "rim_mode_rule": "tom >= 16, tom > 2 * snare, and tom span >= 60% of audio",
        "tom_activity_span_seconds": tom_span,
        "activation_duration_seconds": activation_duration,
        "transient_tom_candidates": transient_count,
        "effective_energy_boundaries": {
            "Kick": {"ghost": ghost_ratio, "accent": max(accent_ratio, 0.85)},
            "Snare": {"ghost": ghost_ratio, "accent": accent_ratio},
            "Mid Tom": {"ghost": min(ghost_ratio, 0.20), "accent": accent_ratio},
            "Closed HH": {
                "ghost": min(ghost_ratio, 0.40),
                "accent": min(accent_ratio, 0.70),
            },
            "Crash": {"ghost": ghost_ratio, "accent": accent_ratio},
            "Cross-stick / Rim": {"ghost": ghost_ratio, "accent": accent_ratio},
        },
    }


def analyze_bass(audio_path: Path, min_midi: int, max_midi: int) -> list[NoteEvent]:
    try:
        from basic_pitch import ICASSP_2022_MODEL_PATH
        from basic_pitch.inference import Model, predict
    except ImportError as exc:
        raise ModelError("Basic Pitch is missing; use Repair in Jam2 Settings") from exc
    print("Transcribing bass notes with Basic Pitch...")
    try:
        model = Model(ICASSP_2022_MODEL_PATH)
        _, midi, _ = predict(
            str(audio_path), model,
            minimum_frequency=440.0 * 2 ** ((min_midi - 69) / 12),
            maximum_frequency=440.0 * 2 ** ((max_midi - 69) / 12),
        )
    except Exception as exc:
        raise ModelError(f"Basic Pitch bass analysis failed: {exc}") from exc
    notes: list[NoteEvent] = []
    for instrument in midi.instruments:
        for note in instrument.notes:
            if min_midi <= note.pitch <= max_midi:
                notes.append(NoteEvent(note.start, note.end, note.pitch, note.velocity))
    return sorted(notes, key=lambda note: (note.start, note.midi))
