from __future__ import annotations

import json
import statistics
from pathlib import Path
import tempfile
import time

from .jamjar import export_song
from .models import (
    ADTOF_REVISION,
    CHORDMINI_REVISION,
    SONGFORMER_REVISION,
    analyze_bass,
    analyze_chords,
    analyze_chroma_chords,
    analyze_drums,
    analyze_structure,
    configure_local_caches,
    prepare_chord_sources,
    resolve_device,
    separate_stems,
    track_beats,
)
from .paths import CACHE_ROOT, REPORTS_ROOT, SONGS_ROOT, portable_slug
from .postprocess import (
    classify_cymbals, contextualize_bass, contextualize_chords,
    section_timing_diagnostics,
)
from .timing import (
    arrangement_steps, choose_sections, estimate_bpm, fuse_chords_with_bass,
    infer_meter, merge_consecutive_structures, repair_drum_hits,
    shape_drum_dynamics, stabilize_chords,
)
from .types import Analysis, TimedLabel, jsonable
from .wav import inspect_loopback_wav, sha256_file


def taste(args: object) -> Path:
    pipeline_started = time.perf_counter()
    configure_local_caches()
    timings: dict[str, float] = {}
    started = time.perf_counter()
    input_path = Path(args.input)
    info = inspect_loopback_wav(input_path)
    display_name = args.name.strip()
    if not display_name:
        raise ValueError("--name cannot be empty")
    if len(display_name) > 512:
        raise ValueError("--name exceeds Jam2's 512-character title limit")
    slug = portable_slug(display_name)
    target = SONGS_ROOT / slug
    if target.exists():
        raise FileExistsError(f"song already exists; refusing to overwrite: {target}")
    device = resolve_device(args.device)
    source_hash = sha256_file(info.path)
    timings["input_validation_seconds"] = time.perf_counter() - started
    raw: dict[str, object] = {}
    warnings: list[str] = []
    report_root = REPORTS_ROOT / slug
    report_root.mkdir(parents=True, exist_ok=True)

    def checkpoint(completed_stage: str) -> None:
        progress = {
            "format": "jamtaster-progress-v1",
            "status": "running",
            "completed_stage": completed_stage,
            "input": str(info.path),
            "audio_duration_seconds": info.duration,
            "device": device,
            "elapsed_seconds": time.perf_counter() - pipeline_started,
            "timings": timings,
        }
        (report_root / "progress.json").write_text(
            json.dumps(jsonable(progress), indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )

    checkpoint("input_validation")
    work_parent = CACHE_ROOT / "work"
    work_parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="taste-", dir=work_parent) as temporary:
        workspace = Path(temporary)

        started = time.perf_counter()
        stems = separate_stems(info.path, info, workspace, device, args.demucs_model)
        timings["separation_seconds"] = time.perf_counter() - started
        checkpoint("separation")

        started = time.perf_counter()
        beats, downbeats = track_beats(info.path, device, args.beat_model)
        timings["beat_tracking_seconds"] = time.perf_counter() - started
        checkpoint("beat_tracking")
        detected_bpm = estimate_bpm(beats)
        requested_bpm = float(args.bpm) if args.bpm is not None else detected_bpm
        # Jam2's native timing schema stores whole BPM. Use that exact value for
        # quantisation and drift reporting instead of silently rounding only at export.
        bpm = float(round(requested_bpm))
        if not 20 <= bpm <= 400:
            raise ValueError("BPM must be between 20 and 400")
        meter = int(args.meter) if args.meter != "auto" else infer_meter(beats, downbeats)
        if meter < 2 or meter > 12:
            raise ValueError("meter must contain 2 to 12 beats per bar")

        started = time.perf_counter()
        if args.sections == "single":
            structures = [TimedLabel(0.0, info.duration, "verse")]
            raw_structure = {
                "skipped": True,
                "reason": "--sections single does not require song-form analysis",
            }
        else:
            structures, raw_structure = analyze_structure(info.path, device)
            original_structures = list(structures)
            structures, structure_merges = merge_consecutive_structures(structures)
            raw["structure_postprocessing"] = {
                "original": original_structures,
                "merged": structures,
                "merges": structure_merges,
            }
        timings["structure_seconds"] = time.perf_counter() - started
        raw["songformer"] = raw_structure
        checkpoint("structure")

        started = time.perf_counter()
        chord_sources = prepare_chord_sources(stems["other"], stems["bass"], workspace)
        timings["chord_source_preparation_seconds"] = time.perf_counter() - started
        raw["chord_source_preparation"] = {
            "model_source": "untouched Other plus Bass",
            "rejected_variants": "broad EQ and HPSS reduced GuitarSet development accuracy",
            "evidence_source": "beat-synchronous chroma from untouched Other plus Bass",
            "exported_stems_unchanged": True,
        }
        checkpoint("chord_source_preparation")
        started = time.perf_counter()
        chords, raw_chords = analyze_chords(chord_sources["raw"], workspace, device)
        _, raw_chroma = analyze_chroma_chords(chord_sources["raw"], beats)
        chord_nonempty_seconds = sum(
            max(0.0, item.end - item.start)
            for item in chords
            if item.label.strip().upper() not in {"N", "X", "NO_CHORD"}
        )
        chord_coverage = chord_nonempty_seconds / max(1e-9, info.duration)
        if chord_coverage < 0.50:
            chroma_chords, _ = analyze_chroma_chords(chord_sources["raw"], beats)
            raw["chroma_chord_fallback"] = {
                "used": True,
                "chordmini_coverage": chord_coverage,
                "segments": raw_chroma,
            }
            chords = chroma_chords
        else:
            raw["chroma_chord_fallback"] = {
                "used": False,
                "chordmini_coverage": chord_coverage,
            }
        timings["chord_seconds"] = time.perf_counter() - started
        raw["chordmini"] = raw_chords
        raw["chroma_chords"] = {
            "raw_other_plus_bass": raw_chroma,
        }
        checkpoint("chords")

        started = time.perf_counter()
        drums, drum_candidates, drum_model = analyze_drums(
            stems["drums"], workspace, device, args.drum_thresholds,
            args.drum_candidate_scale, args.drum_ghost_energy_ratio,
            args.drum_accent_energy_ratio,
        )
        timings["drum_seconds"] = time.perf_counter() - started
        raw["drum_model"] = drum_model
        checkpoint("drums")

        started = time.perf_counter()
        threshold_values = [float(value.strip()) for value in args.drum_thresholds.split(",")]
        drum_thresholds = dict(zip(
            ("Kick", "Snare", "Mid Tom", "Closed HH", "Crash"), threshold_values
        ))
        drums, drum_repair = repair_drum_hits(
            drums, drum_candidates, beats, bpm, meter, args.drum_division,
            drum_thresholds, args.drum_repair_min_repeats,
            args.drum_repair_neighborhood_bars, not args.no_drum_repair,
            bool(drum_model["rim_mode"]),
        )
        timings["drum_repair_seconds"] = time.perf_counter() - started
        raw["drum_repair"] = drum_repair
        checkpoint("drum_repair")

        started = time.perf_counter()
        drums, drum_dynamics = shape_drum_dynamics(
            drums, beats, bpm, meter, args.drum_division, downbeats
        )
        timings["drum_dynamics_seconds"] = time.perf_counter() - started
        raw["drum_dynamics"] = drum_dynamics
        checkpoint("drum_dynamics")

        started = time.perf_counter()
        bass = analyze_bass(stems["bass"], args.bass_min_midi, args.bass_max_midi)
        onset_intervals = [
            current.start - previous.start
            for previous, current in zip(bass, bass[1:])
            if current.start - previous.start > 0.05
        ]
        bass_irregularity = (
            statistics.pstdev(onset_intervals) / statistics.mean(onset_intervals)
            if len(onset_intervals) >= 2 and statistics.mean(onset_intervals) > 0.0
            else None
        )
        sparse_bass = len(bass) < len(beats) * 0.75
        recovery_needed = sparse_bass and (
            len(bass) < 4 or bass_irregularity is None or bass_irregularity > 0.25
        )
        if recovery_needed:
            fullmix_bass = analyze_bass(
                info.path, args.bass_min_midi, args.bass_max_midi
            )
            # The full mix can recover bass attacks hidden by separation, but
            # its chord fundamentals are otherwise prolific false positives.
            # Establish the performed register from the separated stem and
            # only admit recovery notes within that register.
            ordered_stem_pitches = sorted(note.midi for note in bass)
            if ordered_stem_pitches:
                register_index = min(
                    len(ordered_stem_pitches) - 1,
                    round((len(ordered_stem_pitches) - 1) * 0.90),
                )
                recovery_ceiling = min(
                    args.bass_max_midi, ordered_stem_pitches[register_index] + 2
                )
            else:
                recovery_ceiling = min(args.bass_max_midi, args.bass_min_midi + 18)
            added = 0
            basic_pitch_count = len(bass)
            for candidate in fullmix_bass:
                if candidate.midi > recovery_ceiling or candidate.end - candidate.start < 0.12:
                    continue
                if any(
                    note.midi == candidate.midi and
                    abs(note.start - candidate.start) <= 0.10
                    for note in bass
                ):
                    continue
                bass.append(candidate)
                added += 1
            bass.sort(key=lambda note: (note.start, note.midi))
            raw["bass_fullmix_fallback"] = {
                "used": True,
                "stem_notes": basic_pitch_count,
                "fullmix_candidates": len(fullmix_bass),
                "recovery_register_ceiling_midi": recovery_ceiling,
                "added_notes": added,
                "combined_notes": len(bass),
                "stem_onset_irregularity": bass_irregularity,
            }
        else:
            raw["bass_fullmix_fallback"] = {
                "used": False,
                "stem_notes": len(bass),
                "sparse": sparse_bass,
                "stem_onset_irregularity": bass_irregularity,
                "reason": (
                    "regular_sparse_line_already_complete"
                    if sparse_bass else "stem_note_density_sufficient"
                ),
            }
        timings["bass_seconds"] = time.perf_counter() - started
        checkpoint("bass")

        started = time.perf_counter()
        raw_bass = list(bass)
        bass, bass_context = contextualize_bass(bass, chords)
        chords, chord_fusion = fuse_chords_with_bass(chords, bass)
        chords = stabilize_chords(chords, info.duration)
        raw_context_chords = list(chords)
        chords, chord_context = contextualize_chords(
            chords, [raw_chroma], bass, beats, structures, info.duration
        )
        drums, cymbal_context = classify_cymbals(
            drums, beats, downbeats, structures
        )
        timings["context_postprocessing_seconds"] = time.perf_counter() - started
        raw["chord_bass_fusion"] = chord_fusion
        raw["context_postprocessing"] = {
            "chords": chord_context,
            "bass": bass_context,
            "cymbals": cymbal_context,
            "raw_chords": raw_context_chords,
            "raw_bass": raw_bass,
        }
        checkpoint("context_postprocessing")

        analysis = Analysis(
            beats=beats,
            downbeats=downbeats,
            bpm=bpm,
            beats_per_bar=meter,
            structures=structures,
            chords=chords,
            drums=drums,
            bass=bass,
            drum_candidates=drum_candidates,
            drum_repair=drum_repair,
            drum_division=args.drum_division,
            raw=raw,
            warnings=warnings,
        )
        started = time.perf_counter()
        choices = choose_sections(analysis, info.duration, args.sections)
        arrangement = arrangement_steps(structures, choices)
        local_timing = section_timing_diagnostics(choices, beats, bpm)
        raw["local_timing"] = local_timing
        bar_anchored_banks = {
            index for index, bank in enumerate(local_timing)
            if float(local_timing[bank]["max_absolute_residual_ms"]) > args.local_warp_threshold_ms
        }
        for bank, values in local_timing.items():
            if float(values["max_absolute_residual_ms"]) > args.local_drift_warning_ms:
                warnings.append(
                    f"bank {bank} has {values['max_absolute_residual_ms']} ms internal beat residual "
                    "after endpoint stretch; consider a different section boundary or local warp"
                )
        timings["section_selection_seconds"] = time.perf_counter() - started
        checkpoint("section_selection")
        if len(choices) > 12:
            raise ValueError("internal error: more than twelve sections selected")

        staging = workspace / "output"
        started = time.perf_counter()
        song_root, manifest = export_song(
            staging_root=staging,
            display_name=display_name,
            source_hash=source_hash,
            stem_paths=stems,
            analysis=analysis,
            choices=choices,
            arrangement=arrangement,
            arrangement_loop=not args.no_arrangement_loop,
            arrangement_enabled=True,
            time_stretch=not args.no_time_stretch,
            bar_anchored_banks=bar_anchored_banks,
        )
        timings["jamjar_export_seconds"] = time.perf_counter() - started
        checkpoint("jamjar_export")
        for bank in manifest["quantization"].values():
            drift = abs(float(bank["drift_ms"]))
            if drift > args.drift_warning_ms:
                warnings.append(
                    f"bank {bank['role']} exported audio/grid drift is {bank['drift_ms']} ms"
                )

        SONGS_ROOT.mkdir(parents=True, exist_ok=True)
        # A same-volume rename publishes the complete native folder at once.
        started = time.perf_counter()
        song_root.replace(target)
        timings["publish_seconds"] = time.perf_counter() - started
        checkpoint("publish")

    timed_work = sum(value for key, value in timings.items() if key.endswith("_seconds"))
    timings["accounted_work_seconds"] = timed_work
    timings["pipeline_seconds_before_report"] = time.perf_counter() - pipeline_started
    timings["audio_duration_seconds"] = info.duration
    for key, value in list(timings.items()):
        if key.endswith("_seconds") and key not in {
            "audio_duration_seconds", "accounted_work_seconds", "pipeline_seconds_before_report"
        }:
            timings[key.removesuffix("_seconds") + "_realtime_factor"] = value / info.duration

    analysis_report = {
        "format": "jamtaster-analysis-v1",
        "input": {
            "path": str(info.path),
            "sha256": source_hash,
            "sample_rate": info.sample_rate,
            "frames": info.frames,
            "duration_seconds": info.duration,
            "channels": info.channels,
            "sample_width_bytes": info.sample_width,
        },
        "configuration": {
            "device": device,
            "demucs_model": args.demucs_model,
            "beat_model": args.beat_model,
            "sections": args.sections,
            "bpm_override": args.bpm,
            "meter": args.meter,
            "drum_thresholds": args.drum_thresholds,
            "drum_candidate_scale": args.drum_candidate_scale,
            "drum_division": args.drum_division,
            "drum_ghost_energy_ratio": args.drum_ghost_energy_ratio,
            "drum_accent_energy_ratio": args.drum_accent_energy_ratio,
            "drum_repair_min_repeats": args.drum_repair_min_repeats,
            "drum_repair_neighborhood_bars": args.drum_repair_neighborhood_bars,
            "drum_repair": not args.no_drum_repair,
            "bass_min_midi": args.bass_min_midi,
            "bass_max_midi": args.bass_max_midi,
            "drift_warning_ms": args.drift_warning_ms,
            "time_stretch": not args.no_time_stretch,
            "arrangement_enabled": True,
        },
        "detected_bpm": detected_bpm,
        "analysis": jsonable(analysis),
        "selected_sections": jsonable(choices),
        "arrangement": arrangement,
        "timings": timings,
        "quantization": manifest["quantization"],
        "warnings": warnings,
    }
    manifest.update({
        "format": "jamtaster-manifest-v1",
        "display_name": display_name,
        "source_sha256": source_hash,
        "song_folder": str(target),
        "report_folder": str(report_root),
        "models": {
            "demucs": args.demucs_model,
            "beat_this": args.beat_model,
            "songformer_revision": SONGFORMER_REVISION,
            "chordmini_revision": CHORDMINI_REVISION,
            "adtof_revision": ADTOF_REVISION,
            "basic_pitch": "0.4.0",
        },
    })
    started = time.perf_counter()
    (report_root / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    timings["report_write_seconds"] = time.perf_counter() - started
    timings["total_seconds"] = time.perf_counter() - pipeline_started
    timings["total_realtime_factor"] = timings["total_seconds"] / info.duration
    progress_path = report_root / "progress.json"
    progress_path.write_text(
        json.dumps({
            "format": "jamtaster-progress-v1",
            "status": "complete",
            "completed_stage": "reports",
            "input": str(info.path),
            "audio_duration_seconds": info.duration,
            "device": device,
            "elapsed_seconds": timings["total_seconds"],
            "timings": timings,
        }, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    (report_root / "analysis.json").write_text(
        json.dumps(analysis_report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return target
