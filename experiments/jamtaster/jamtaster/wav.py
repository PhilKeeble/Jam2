from __future__ import annotations

import hashlib
from pathlib import Path
import wave

from .types import WavInfo


class WavError(ValueError):
    pass


def inspect_loopback_wav(path: Path) -> WavInfo:
    path = path.expanduser().resolve()
    if path.suffix.lower() != ".wav":
        raise WavError("the first POC accepts .wav input only")
    if not path.is_file():
        raise WavError(f"input WAV does not exist: {path}")
    try:
        with wave.open(str(path), "rb") as source:
            if source.getcomptype() != "NONE":
                raise WavError("input must be uncompressed PCM WAV")
            info = WavInfo(
                path=path,
                sample_rate=source.getframerate(),
                frames=source.getnframes(),
                channels=source.getnchannels(),
                sample_width=source.getsampwidth(),
            )
    except (wave.Error, EOFError) as exc:
        raise WavError(f"invalid RIFF/WAVE file: {exc}") from exc
    if info.channels != 1:
        raise WavError(f"expected Jam2 loopback mono WAV, found {info.channels} channels")
    if info.sample_width != 2:
        raise WavError(
            f"expected Jam2 loopback PCM16 WAV, found {info.sample_width * 8}-bit samples"
        )
    if info.sample_rate < 8000 or info.sample_rate > 384000:
        raise WavError(f"unsupported sample rate: {info.sample_rate} Hz")
    if info.frames <= 0:
        raise WavError("input WAV is empty")
    return info


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(chunk_size), b""):
            digest.update(chunk)
    return digest.hexdigest()


def crop_pcm16_mono(source_path: Path, destination: Path, start: float, end: float) -> int:
    """Copy an exact frame-aligned crop without transcoding or changing sample rate."""
    if end <= start:
        raise WavError("crop end must be after crop start")
    with wave.open(str(source_path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise WavError(f"intermediate stem is not mono PCM16: {source_path}")
        rate = source.getframerate()
        first = max(0, min(source.getnframes(), round(start * rate)))
        last = max(first + 1, min(source.getnframes(), round(end * rate)))
        source.setpos(first)
        remaining = last - first
        destination.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(destination), "wb") as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(rate)
            while remaining:
                count = min(remaining, 65536)
                data = source.readframes(count)
                if not data:
                    break
                target.writeframesraw(data)
                remaining -= len(data) // 2
            target.writeframes(b"")
    return last - first


def crop_and_stretch_pcm16_mono(
    source_path: Path,
    destination: Path,
    start: float,
    end: float,
    target_frames: int,
    enabled: bool = True,
) -> dict[str, int | float | bool | str]:
    """Crop a mono PCM16 section and pitch-preserve it to an exact frame count."""
    if target_frames <= 0:
        raise WavError("stretch target must contain at least one frame")
    with wave.open(str(source_path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise WavError(f"intermediate stem is not mono PCM16: {source_path}")
        rate = source.getframerate()
        first = max(0, min(source.getnframes(), round(start * rate)))
        last = max(first + 1, min(source.getnframes(), round(end * rate)))
        source.setpos(first)
        pcm = source.readframes(last - first)
    input_frames = len(pcm) // 2
    if input_frames <= 0:
        raise WavError(f"cropped stem is empty: {source_path}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    applied = enabled and input_frames != target_frames
    if applied:
        try:
            import numpy as np
            import python_stretch as ps
        except ImportError as exc:
            raise WavError(
                "python-stretch is missing; run JamTaster.py setup"
            ) from exc
        values = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        stretcher = ps.Signalsmith.Stretch()
        stretcher.preset(1, rate)
        stretcher.timeFactor = input_frames / target_frames
        stretched = np.asarray(
            stretcher.process(values[np.newaxis, :])[0], dtype=np.float32
        )
        produced_frames = len(stretched)
        if produced_frames < target_frames:
            stretched = np.pad(stretched, (0, target_frames - produced_frames))
        elif produced_frames > target_frames:
            stretched = stretched[:target_frames]
        output_pcm = np.rint(np.clip(stretched, -1.0, 32767.0 / 32768.0) * 32768.0)
        pcm = output_pcm.astype("<i2").tobytes()
    else:
        produced_frames = input_frames

    with wave.open(str(destination), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(rate)
        target.writeframes(pcm)
    return {
        "algorithm": "signalsmith" if applied else "exact-copy",
        "enabled": enabled,
        "applied": applied,
        "sample_rate": rate,
        "input_frames": input_frames,
        "target_frames": target_frames,
        "processor_frames": produced_frames,
        "output_frames": len(pcm) // 2,
        "time_factor": input_frames / target_frames,
        "source_seconds": input_frames / rate,
        "target_seconds": target_frames / rate,
    }


def crop_and_stretch_pcm16_mono_bar_anchored(
    source_path: Path,
    destination: Path,
    boundaries: list[float],
    target_frames: int,
) -> dict[str, object]:
    """Pitch-preserve each tracked bar to its share of one exact Jam2 grid."""
    if len(boundaries) < 2 or any(b <= a for a, b in zip(boundaries, boundaries[1:])):
        raise WavError("bar-anchored stretch requires increasing source boundaries")
    try:
        import numpy as np
        import python_stretch as ps
    except ImportError as exc:
        raise WavError("python-stretch is missing; run JamTaster.py setup") from exc
    with wave.open(str(source_path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise WavError(f"intermediate stem is not mono PCM16: {source_path}")
        rate = source.getframerate()
        total_source_frames = source.getnframes()
        frame_boundaries = [
            max(0, min(total_source_frames, round(value * rate)))
            for value in boundaries
        ]
        if any(b <= a for a, b in zip(frame_boundaries, frame_boundaries[1:])):
            raise WavError("bar-anchored stretch produced an empty source segment")
        output_parts = []
        segments = []
        processor_frames = 0
        segment_count = len(frame_boundaries) - 1
        for index, (first, last) in enumerate(zip(frame_boundaries, frame_boundaries[1:])):
            source.setpos(first)
            pcm = source.readframes(last - first)
            input_frames = len(pcm) // 2
            target_first = round(target_frames * index / segment_count)
            target_last = round(target_frames * (index + 1) / segment_count)
            segment_target = target_last - target_first
            values = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
            stretcher = ps.Signalsmith.Stretch()
            stretcher.preset(1, rate)
            stretcher.timeFactor = input_frames / segment_target
            stretched = np.asarray(
                stretcher.process(values[np.newaxis, :])[0], dtype=np.float32
            )
            produced = len(stretched)
            processor_frames += produced
            if produced < segment_target:
                stretched = np.pad(stretched, (0, segment_target - produced))
            elif produced > segment_target:
                stretched = stretched[:segment_target]
            output_parts.append(stretched)
            segments.append({
                "index": index,
                "source_start": boundaries[index],
                "source_end": boundaries[index + 1],
                "input_frames": input_frames,
                "target_frames": segment_target,
                "time_factor": input_frames / segment_target,
            })
    output = np.concatenate(output_parts) if output_parts else np.empty(0, dtype=np.float32)
    pcm = np.rint(np.clip(output, -1.0, 32767.0 / 32768.0) * 32768.0).astype("<i2").tobytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(destination), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(rate)
        target.writeframes(pcm)
    input_frames = frame_boundaries[-1] - frame_boundaries[0]
    return {
        "algorithm": "signalsmith-bar-anchored",
        "enabled": True,
        "applied": True,
        "sample_rate": rate,
        "input_frames": input_frames,
        "target_frames": target_frames,
        "processor_frames": processor_frames,
        "output_frames": len(pcm) // 2,
        "time_factor": input_frames / target_frames,
        "source_seconds": input_frames / rate,
        "target_seconds": target_frames / rate,
        "segments": segments,
    }
