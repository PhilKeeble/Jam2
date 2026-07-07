#!/usr/bin/env python3

import json
import math
import wave
from pathlib import Path


STEMS = ("mix", "my-input", "their-input", "inputs-mix", "metronome")
INT16_FULL_SCALE = 32768.0


def read_wav_mono(path):
    path = Path(path)
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)
    if channels != 1 or sample_width != 2:
        raise ValueError(f"{path} must be mono 16-bit PCM WAV")
    samples = []
    for i in range(0, len(raw), 2):
        value = int.from_bytes(raw[i:i + 2], "little", signed=True)
        samples.append(value / INT16_FULL_SCALE)
    return {"path": str(path), "sample_rate": sample_rate, "samples": samples}


def _percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((len(ordered) - 1) * pct))))
    return ordered[index]


def basic_stats(samples):
    if not samples:
        return {
            "frames": 0,
            "peak": 0.0,
            "rms": 0.0,
            "clipped_frames": 0,
            "pop_events": 0,
            "dropout_runs": 0,
            "noise_floor_p95": 0.0,
        }
    peak = max(abs(sample) for sample in samples)
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
    clipped = sum(1 for sample in samples if abs(sample) >= 0.999)
    diffs = [abs(samples[i] - samples[i - 1]) for i in range(1, len(samples))]
    pop_events = sum(1 for diff in diffs if diff >= 0.45)
    dropout_runs = 0
    run = 0
    for sample in samples:
        if abs(sample) < 0.0001:
            run += 1
        else:
            if run >= 2048:
                dropout_runs += 1
            run = 0
    if run >= 2048:
        dropout_runs += 1
    return {
        "frames": len(samples),
        "peak": peak,
        "rms": rms,
        "clipped_frames": clipped,
        "pop_events": pop_events,
        "dropout_runs": dropout_runs,
        "noise_floor_p95": _percentile([abs(sample) for sample in samples], 0.95),
    }


def estimate_tone(samples, sample_rate, expected_hz=440.0):
    if not samples or sample_rate <= 0:
        return {"tone_present": False}
    crossings = []
    previous = samples[0]
    for index, sample in enumerate(samples[1:], start=1):
        if previous < 0.0 <= sample:
            crossings.append(index)
        previous = sample
    if len(crossings) < 4:
        return {"tone_present": False, "zero_crossings": len(crossings)}
    intervals = [crossings[i] - crossings[i - 1] for i in range(1, len(crossings))]
    median = sorted(intervals)[len(intervals) // 2]
    hz = sample_rate / median if median > 0 else 0.0
    return {
        "tone_present": True,
        "estimated_hz": hz,
        "expected_hz": expected_hz,
        "error_hz": hz - expected_hz,
        "zero_crossings": len(crossings),
    }


def detect_transients(samples, threshold=0.10, refractory_frames=512):
    frames = []
    last = -refractory_frames
    for index, sample in enumerate(samples):
        if abs(sample) >= threshold and index - last >= refractory_frames:
            frames.append(index)
            last = index
    return frames


def _expected_metronome_frames(meta, sample_count):
    sample_rate = int(meta.get("sample_rate", 48000) or 48000)
    bpm = int(meta.get("bpm", 120) or 120)
    division = int(meta.get("metronome_division", 1) or 1)
    step_count = max(1, int(meta.get("metronome_step_count", 4) or 4))
    play_low = int(meta.get("metronome_play_mask_low", 0x0f) or 0)
    play_high = int(meta.get("metronome_play_mask_high", 0) or 0)
    start_audio_frame = int(meta.get("start_audio_frame", 0) or 0)
    epoch = int(meta.get("metronome_epoch_sample_time", 0) or 0)
    epoch_valid = bool(meta.get("metronome_epoch_valid", False))
    step_interval = int(round(sample_rate * 60.0 / max(1, bpm) / max(1, division)))
    if not epoch_valid or step_interval <= 0:
        return []
    frames = []
    stop_audio_frame = start_audio_frame + sample_count
    first_absolute = max(start_audio_frame, epoch)
    step = max(0, math.floor((first_absolute - epoch) / step_interval) - 1)
    while True:
        absolute = epoch + step * step_interval
        if absolute >= stop_audio_frame:
            break
        if absolute >= start_audio_frame:
            pattern_step = step % step_count
            mask = play_low if pattern_step < 64 else play_high
            bit = pattern_step if pattern_step < 64 else pattern_step - 64
            if ((mask >> bit) & 1) != 0:
                frames.append(absolute - start_audio_frame)
        step += 1
    return frames


def analyze_metronome_wav(recording_dir, tolerance_frames=96, allow_silent=False):
    recording_dir = Path(recording_dir)
    sidecar = recording_dir / "recording.json"
    wav_path = recording_dir / "metronome.wav"
    if not wav_path.exists():
        return {"ok": False, "verdict": "metronome_wav_missing", "recording_dir": str(recording_dir)}
    meta = {}
    if sidecar.exists():
        meta = json.loads(sidecar.read_text(encoding="utf-8"))
    wav = read_wav_mono(wav_path)
    samples = wav["samples"]
    actual = detect_transients(samples, threshold=0.04, refractory_frames=1024)
    if allow_silent and not actual:
        return {"ok": True, "verdict": "metronome_silent_expected", "clicks": 0, "recording_dir": str(recording_dir)}
    expected = _expected_metronome_frames({**meta, "sample_rate": wav["sample_rate"]}, len(samples))
    errors = []
    used = set()
    for expected_frame in expected:
        best_index = None
        best_error = None
        for index, actual_frame in enumerate(actual):
            if index in used:
                continue
            error = abs(actual_frame - expected_frame)
            if best_error is None or error < best_error:
                best_index = index
                best_error = error
        if best_index is not None and best_error is not None and best_error <= tolerance_frames:
            used.add(best_index)
            errors.append(best_error)
    missing = max(0, len(expected) - len(errors))
    extra = max(0, len(actual) - len(used))
    max_error = max(errors, default=0)
    startup_boundary_mismatch = False
    steady_missing = missing
    steady_extra = extra
    steady_max_error = max_error
    if missing or extra:
        for actual_start in range(1, min(3, len(actual)) + 1):
            if len(expected) <= 1 or len(expected[1:]) != len(actual[actual_start:]):
                continue
            steady_errors = [abs(actual_frame - expected_frame)
                             for expected_frame, actual_frame in zip(expected[1:], actual[actual_start:])]
            if steady_errors and all(error <= tolerance_frames for error in steady_errors):
                startup_boundary_mismatch = True
                steady_missing = 0
                steady_extra = 0
                steady_max_error = max(steady_errors, default=0)
                break
    ok = steady_missing == 0 and steady_extra == 0 and steady_max_error <= tolerance_frames
    if ok:
        verdict = "pass_startup_boundary" if startup_boundary_mismatch else "pass"
    else:
        verdict = "metronome_click_count_mismatch" if steady_missing or steady_extra else "metronome_click_timing_high"
    return {
        "ok": ok,
        "verdict": verdict,
        "recording_dir": str(recording_dir),
        "expected_clicks": len(expected),
        "actual_clicks": len(actual),
        "missing_clicks": missing,
        "extra_clicks": extra,
        "startup_boundary_mismatch": startup_boundary_mismatch,
        "steady_missing_clicks": steady_missing,
        "steady_extra_clicks": steady_extra,
        "steady_max_abs_error_frames": steady_max_error,
        "max_abs_error_frames": max_error,
        "tolerance_frames": tolerance_frames,
    }


def _signal_for_stem(stem, local_signal, remote_signal, mix_signal):
    if stem == "my-input":
        return local_signal
    if stem == "their-input":
        return remote_signal
    return mix_signal


def analyze_recording_dir(recording_dir, signal="silence", local_signal=None, remote_signal=None):
    recording_dir = Path(recording_dir)
    local_signal = local_signal or signal
    remote_signal = remote_signal or signal
    mix_signal = "tone-440" if "tone-440" in (local_signal, remote_signal) else signal
    result = {"recording_dir": str(recording_dir), "ok": True, "tags": [], "stems": {}}
    sidecar_path = recording_dir / "recording.json"
    if sidecar_path.exists():
        try:
            result["sidecar"] = json.loads(sidecar_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            result["tags"].append("recording_sidecar_invalid")
    else:
        result["tags"].append("recording_sidecar_missing")
    lengths = []
    for stem in STEMS:
        path = recording_dir / f"{stem}.wav"
        if not path.exists():
            result["stems"][stem] = {"exists": False}
            result["tags"].append(f"{stem}_missing")
            continue
        try:
            wav = read_wav_mono(path)
        except Exception as error:
            result["stems"][stem] = {"exists": False, "error": str(error)}
            result["tags"].append(f"{stem}_invalid")
            continue
        stats = basic_stats(wav["samples"])
        stats.update({"exists": True, "sample_rate": wav["sample_rate"], "path": wav["path"]})
        expected_signal = _signal_for_stem(stem, local_signal, remote_signal, mix_signal)
        if expected_signal == "tone-440" and stem in ("my-input", "their-input", "mix", "inputs-mix"):
            stats["tone"] = estimate_tone(wav["samples"], wav["sample_rate"], 440.0)
        if expected_signal == "pulse-1s" and stem in ("my-input", "their-input", "mix", "inputs-mix"):
            stats["pulse_frames"] = detect_transients(wav["samples"], threshold=0.08, refractory_frames=max(512, wav["sample_rate"] // 2))
        result["stems"][stem] = stats
        lengths.append(stats["frames"])
        if stats["clipped_frames"] > 0:
            result["tags"].append(f"{stem}_clipping_detected")
        if stats["pop_events"] > 0:
            result["tags"].append(f"{stem}_pop_detected")
    if lengths and max(lengths) - min(lengths) > 1:
        result["tags"].append("stem_length_mismatch")
    sidecar = result.get("sidecar", {})
    if sidecar.get("dropped_frames", 0):
        result["tags"].append("recording_dropped_frames")
    if sidecar.get("writer_errors", 0):
        result["tags"].append("recording_writer_errors")
    if local_signal == "silence":
        stats = result["stems"].get("my-input", {})
        if stats.get("rms", 0.0) > 0.002 or stats.get("peak", 0.0) > 0.05:
            result["tags"].append("my-input_unexpected_signal")
    if remote_signal == "silence":
        stats = result["stems"].get("their-input", {})
        if stats.get("rms", 0.0) > 0.002 or stats.get("peak", 0.0) > 0.05:
            result["tags"].append("their-input_unexpected_signal")
    for stem, expected_signal in (("my-input", local_signal), ("their-input", remote_signal)):
        if expected_signal == "tone-440":
            tone = result["stems"].get(stem, {}).get("tone", {})
            if not tone.get("tone_present", False):
                result["tags"].append(f"{stem}_tone_missing")
            elif abs(tone.get("error_hz", 0.0)) > 5.0:
                result["tags"].append(f"{stem}_tone_frequency_mismatch")
    if signal == "metronome-only":
        metro = analyze_metronome_wav(recording_dir)
        result["metronome"] = metro
        if not metro.get("ok", False):
            result["tags"].append(metro.get("verdict", "metronome_wav_failed"))
    result["ok"] = not result["tags"]
    return result
