from __future__ import annotations

import hashlib
import json
from pathlib import Path
import uuid

from .paths import portable_slug
from .timing import quantize_drums, quantize_musical
from .types import Analysis, SectionChoice
from .wav import (
    crop_and_stretch_pcm16_mono,
    crop_and_stretch_pcm16_mono_bar_anchored,
    inspect_loopback_wav,
    sha256_file,
)


MAX_JAMJAR_BYTES = 4 * 1024 * 1024
MAX_ASSET_BYTES = 512 * 1024 * 1024
ASSET_NAMESPACE = uuid.UUID("9552b671-ef33-43b8-8884-1b7f3ec0053a")
STEM_ORDER = ("drums", "bass", "other", "vocals")
STEM_LANE_NAMES = {
    "drums": "Drums",
    "bass": "Bass",
    "other": "Chords / Other",
    "vocals": "Vocals",
}


def _timing(bpm: float, beats_per_bar: int, inherits: bool) -> dict[str, object]:
    bpm_int = max(20, min(400, round(bpm)))
    mask = (1 << min(64, beats_per_bar)) - 1
    return {
        "version": 1,
        "inherits_bank_a": inherits,
        "bpm": bpm_int,
        "beats_per_bar": beats_per_bar,
        "beat_unit": 4,
        "tempo_pulse_units": 1,
        "division": 1,
        "play_mask_low": str(mask),
        "play_mask_high": "0",
        "accent_mask_low": "1",
        "accent_mask_high": "0",
    }


def _empty_step(state: str = "rest") -> dict[str, object]:
    return {"state": state, "value": "", "velocity": 88, "articulation": "", "voicing": ""}


def _empty_section(index: int, beats: int = 8) -> dict[str, object]:
    label = chr(ord("A") + index)
    musical = {
        "division": 1,
        "chords": [_empty_step()],
        "melody": [_empty_step()],
        "bass": [_empty_step()],
        "support": [_empty_step()],
    }
    return {
        "id": str(uuid.uuid5(ASSET_NAMESPACE, f"empty-section-{index}")),
        "label": label,
        "name": f"Unused {label}",
        "beats": beats,
        "chords": ["-"] * beats,
        "targets": [""] * beats,
        "beat_notes": [""] * beats,
        "lyrics": [""] * beats,
        "beat_patterns": [{"division": 4, "lanes": ["...."] * 10} for _ in range(beats)],
        "musical_patterns": [musical for _ in range(beats)],
        "drum_kit": "acoustic",
        "generated_kind": "",
    }


def _choice_name(choice: SectionChoice) -> str:
    return choice.source_label.strip() or f"Section {choice.role}"


def _track(bpm: float, beats_per_bar: int) -> dict[str, object]:
    bpm_int = max(20, min(400, round(bpm)))
    return {
        "file_path": "",
        "file_name": "No backing track",
        "sha256": "",
        "file_bytes": 0,
        "duration_ms": 0,
        "sample_rate": 0,
        "guessed_bpm": 0,
        "accepted_bpm": bpm_int,
        "key": "Unknown",
        "speed": 1,
        "pitch_cents": 0,
        "loop_enabled": True,
        "loop_start_seconds": -1,
        "loop_end_seconds": -1,
        "track_gain_db": -3,
        "focus_enabled": False,
        "focus_preset": "custom",
        "focus_frequency_hz": 120,
        "focus_gain_db": 12,
        "focus_q": 6,
        "highpass_hz": 40,
        "lowpass_hz": 400,
        "metronome_bpm": bpm_int,
        "metronome_beats": beats_per_bar,
        "metronome_beat_unit": 4,
        "metronome_tempo_pulse_units": 1,
        "metronome_division": 1,
        "metronome_click_enabled": [True] * beats_per_bar,
        "metronome_click_accents": [True] + [False] * (beats_per_bar - 1),
    }


def _stable_id(*parts: object) -> str:
    return str(uuid.uuid5(ASSET_NAMESPACE, "|".join(str(part) for part in parts)))


def _lane(asset_path: str, asset_hash: str, name: str, sample_rate: int, frames: int, lane_id: str) -> dict[str, object]:
    return {
        "id": lane_id,
        "asset_path": asset_path,
        "asset_hash": asset_hash,
        "name": name,
        "sample_rate": sample_rate,
        "source_frames": str(frames),
        "start_frame": "0",
        "stop_frame": str(frames),
        "loop_start_frame": "-1",
        "loop_end_frame": "-1",
        "loop_enabled": False,
        "gain_db": 0,
        "muted": False,
        "solo": False,
        "local_only": False,
        "origin_kind": "imported",
        "reference_kind": "",
        "reference_source_signature": "",
        "reference_bpm": 0,
        "reference_stale": False,
    }


def export_song(
    *,
    staging_root: Path,
    display_name: str,
    source_hash: str,
    stem_paths: dict[str, Path],
    analysis: Analysis,
    choices: list[SectionChoice],
    arrangement: list[dict[str, int]],
    arrangement_loop: bool,
    arrangement_enabled: bool = True,
    time_stretch: bool = True,
    bar_anchored_banks: set[int] | None = None,
) -> tuple[Path, dict[str, object]]:
    slug = portable_slug(display_name)
    song_root = staging_root / slug
    imported = song_root / "imported"
    imported.mkdir(parents=True)
    sections: list[dict[str, object]] = []
    banks: list[dict[str, object]] = []
    assets: list[dict[str, object]] = []
    quantization: dict[str, object] = {}
    bar_anchored_banks = bar_anchored_banks or set()

    if len(choices) > 12:
        raise ValueError("Jam2 supports at most 12 sections")
    bank_count = max(4, len(choices))
    for bank_index in range(bank_count):
        label = chr(ord("A") + bank_index)
        if bank_index >= len(choices):
            sections.append(_empty_section(bank_index))
            banks.append({
                "id": label,
                "lanes": [],
                "timing": _timing(analysis.bpm, analysis.beats_per_bar, bank_index > 0),
            })
            continue
        choice = choices[bank_index]
        drum_patterns, drum_diagnostics = quantize_drums(
            analysis.drums, choice, analysis.beats, analysis.bpm, analysis.drum_division
        )
        musical_patterns, legacy_chords, chord_diagnostics = quantize_musical(
            analysis.chords, analysis.bass, choice, analysis.beats, analysis.bpm
        )
        sections.append({
            "id": _stable_id(source_hash, "section", choice.role, round(choice.start, 6), round(choice.end, 6)),
            "label": label,
            "name": _choice_name(choice),
            "beats": choice.beats,
            "chords": legacy_chords,
            "targets": [""] * choice.beats,
            "beat_notes": [""] * choice.beats,
            "lyrics": [""] * choice.beats,
            "beat_patterns": drum_patterns,
            "musical_patterns": musical_patterns,
            "drum_kit": "acoustic",
            "generated_kind": "",
        })
        lanes: list[dict[str, object]] = []
        bank_stretches: list[dict[str, int | float | bool | str]] = []
        for stem in STEM_ORDER:
            source_path = stem_paths[stem]
            stable = hashlib.sha256(
                f"{source_hash}|{choice.start:.6f}|{choice.end:.6f}|{stem}".encode("utf-8")
            ).hexdigest()[:12]
            filename = f"jamtaster-{label.lower()}-{choice.role}-{stem}-{stable}.wav"
            destination = imported / filename
            source_info = inspect_loopback_wav(source_path)
            target_frames = round(
                choice.beats * 60.0 * source_info.sample_rate / analysis.bpm
            )
            if time_stretch and bank_index in bar_anchored_banks:
                boundaries = [choice.start]
                boundaries.extend(
                    analysis.beats[choice.first_beat + offset]
                    for offset in range(
                        analysis.beats_per_bar, choice.beats, analysis.beats_per_bar
                    )
                    if choice.first_beat + offset < len(analysis.beats)
                )
                boundaries.append(choice.end)
                stretch = crop_and_stretch_pcm16_mono_bar_anchored(
                    source_path, destination, boundaries, target_frames
                )
            else:
                stretch = crop_and_stretch_pcm16_mono(
                    source_path,
                    destination,
                    choice.start,
                    choice.end,
                    target_frames,
                    enabled=time_stretch,
                )
            frames = int(stretch["output_frames"])
            bank_stretches.append(stretch)
            if destination.stat().st_size > MAX_ASSET_BYTES:
                raise ValueError(
                    f"cropped {stem} asset exceeds Jam2's {MAX_ASSET_BYTES}-byte limit"
                )
            info = inspect_loopback_wav(destination)
            asset_hash = sha256_file(destination)
            relative = f"imported/{filename}"
            lane_name = f"{label} {_choice_name(choice)} - {STEM_LANE_NAMES[stem]}"
            lanes.append(_lane(
                relative,
                asset_hash,
                lane_name,
                info.sample_rate,
                frames,
                _stable_id(source_hash, label, stem, stable),
            ))
            assets.append({
                "bank": label,
                "role": choice.role,
                "stem": stem,
                "path": relative,
                "sha256": asset_hash,
                "sample_rate": info.sample_rate,
                "frames": frames,
                "stretch": stretch,
            })
        representative = bank_stretches[0]
        fixed_grid_seconds = choice.beats * 60.0 / analysis.bpm
        output_audio_seconds = (
            int(representative["output_frames"]) /
            int(representative["sample_rate"])
        )
        source_audio_seconds = (
            int(representative["input_frames"]) /
            int(representative["sample_rate"])
        )
        quantization[label] = {
            "role": choice.role,
            "drum_hits": drum_diagnostics,
            "chords": chord_diagnostics,
            "fixed_grid_seconds": fixed_grid_seconds,
            "source_audio_seconds": source_audio_seconds,
            "output_audio_seconds": output_audio_seconds,
            "source_drift_ms": round(
                (source_audio_seconds - fixed_grid_seconds) * 1000.0, 3
            ),
            "drift_ms": round(
                (output_audio_seconds - fixed_grid_seconds) * 1000.0, 3
            ),
            "time_stretch_enabled": time_stretch,
            "time_factor": representative["time_factor"],
        }
        banks.append({
            "id": label,
            "lanes": lanes,
            "timing": _timing(analysis.bpm, analysis.beats_per_bar, bank_index > 0),
        })

    jamjar = {
        "beat_lane_schema": 3,
        "title": display_name.strip(),
        "guitar_strings": 6,
        "guitar_drop_tuning": False,
        "sections": sections,
        "looper": {
            "active_bank": 0,
            "grid_lock": True,
            "banks": banks,
            "arrangement": {
                "version": 1,
                "enabled": arrangement_enabled,
                "loop": arrangement_loop,
                "steps": arrangement[:64],
            },
        },
        "track": _track(analysis.bpm, analysis.beats_per_bar),
    }
    jamjar_path = song_root / f"{slug}.jamjar"
    encoded = (json.dumps(jamjar, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    if len(encoded) > MAX_JAMJAR_BYTES:
        raise ValueError(
            f"generated JamJar is {len(encoded)} bytes, exceeding Jam2's {MAX_JAMJAR_BYTES}-byte limit"
        )
    jamjar_path.write_bytes(encoded)
    return song_root, {
        "song_slug": slug,
        "jamjar": jamjar_path.name,
        "jamjar_bytes": len(encoded),
        "assets": assets,
        "quantization": quantization,
    }
