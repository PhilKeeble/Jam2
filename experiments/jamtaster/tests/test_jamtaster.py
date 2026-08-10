from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from jamtaster.jamjar import export_song
from jamtaster.paths import portable_slug
from jamtaster.postprocess import (
    classify_cymbals,
    contextualize_bass,
    contextualize_chords,
    section_timing_diagnostics,
)
from jamtaster.timing import (
    arrangement_steps,
    choose_sections,
    drum_state,
    fuse_chords_with_bass,
    merge_consecutive_structures,
    normalize_chord,
    quantize_drums,
    quantize_musical,
    repair_drum_hits,
    shape_drum_dynamics,
    stabilize_chords,
    structure_role,
)
from jamtaster.types import Analysis, DrumHit, NoteEvent, SectionChoice, TimedLabel, jsonable
from jamtaster.wav import (
    WavError, crop_and_stretch_pcm16_mono,
    crop_and_stretch_pcm16_mono_bar_anchored,
    crop_pcm16_mono, inspect_loopback_wav,
)


def write_silence(path: Path, *, seconds: float = 2.0, rate: int = 48000, channels: int = 1, width: int = 2) -> None:
    with wave.open(str(path), "wb") as target:
        target.setnchannels(channels)
        target.setsampwidth(width)
        target.setframerate(rate)
        target.writeframes(b"\0" * round(seconds * rate) * channels * width)


class PortableSlugTests(unittest.TestCase):
    def test_matches_native_rules(self) -> None:
        self.assertEqual(portable_slug("  My   Song.  "), "My_Song")
        self.assertEqual(portable_slug("a<b>:c"), "a_b_c")
        self.assertEqual(portable_slug("CON"), "_CON")
        self.assertEqual(portable_slug("..."), "Untitled_Jam")
        self.assertEqual(len(portable_slug("x" * 200)), 120)

    def test_jsonable_converts_model_scalars(self) -> None:
        class ModelScalar:
            def item(self) -> int:
                return 7

        self.assertEqual(jsonable({"value": ModelScalar()}), {"value": 7})


class WavTests(unittest.TestCase):
    def test_accepts_loopback_shape_and_crops_frames(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "source.wav"
            output = Path(folder) / "crop.wav"
            write_silence(source)
            info = inspect_loopback_wav(source)
            self.assertEqual((info.channels, info.sample_width, info.sample_rate), (1, 2, 48000))
            frames = crop_pcm16_mono(source, output, 0.25, 0.75)
            self.assertEqual(frames, 24000)
            self.assertEqual(inspect_loopback_wav(output).frames, 24000)

    def test_rejects_stereo(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "stereo.wav"
            write_silence(source, channels=2)
            with self.assertRaisesRegex(WavError, "mono"):
                inspect_loopback_wav(source)

    def test_signalsmith_stretch_hits_exact_target_frame(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "source.wav"
            output = Path(folder) / "stretched.wav"
            write_silence(source, seconds=1.01)
            result = crop_and_stretch_pcm16_mono(
                source, output, 0.0, 1.01, 48000, enabled=True
            )
            self.assertTrue(result["applied"])
            self.assertEqual(result["algorithm"], "signalsmith")
            self.assertEqual(inspect_loopback_wav(output).frames, 48000)

    def test_disabled_stretch_keeps_original_crop_duration(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "source.wav"
            output = Path(folder) / "copied.wav"
            write_silence(source, seconds=1.01)
            result = crop_and_stretch_pcm16_mono(
                source, output, 0.0, 1.01, 48000, enabled=False
            )
            self.assertFalse(result["applied"])
            self.assertEqual(inspect_loopback_wav(output).frames, 48480)

    def test_bar_anchored_stretch_hits_exact_total_and_segment_frames(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "source.wav"
            output = Path(folder) / "bar-stretched.wav"
            write_silence(source, seconds=2.04)
            result = crop_and_stretch_pcm16_mono_bar_anchored(
                source, output, [0.0, 1.01, 2.04], 96000
            )
            self.assertEqual(result["algorithm"], "signalsmith-bar-anchored")
            self.assertEqual(result["output_frames"], 96000)
            self.assertEqual([item["target_frames"] for item in result["segments"]], [48000, 48000])


class TimingTests(unittest.TestCase):
    def test_consecutive_equal_structure_labels_merge_but_later_verse_does_not(self) -> None:
        structures = [
            TimedLabel(0, 4, "verse"), TimedLabel(4, 8, "Verse"),
            TimedLabel(8, 12, "chorus"), TimedLabel(12, 16, "verse"),
        ]
        merged, diagnostics = merge_consecutive_structures(structures)
        self.assertEqual(
            [(item.start, item.end, item.label) for item in merged],
            [(0, 8, "verse"), (8, 12, "chorus"), (12, 16, "verse")],
        )
        self.assertEqual(len(diagnostics), 1)

    def test_context_chords_remove_isolated_lead_note_excursion(self) -> None:
        primary = [
            TimedLabel(0, 1, "C"), TimedLabel(1, 2, "D"), TimedLabel(2, 3, "C")
        ]
        profile = [1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0]
        chroma = [
            {
                "start": index, "end": index + 1, "label": "C", "profile": profile,
                "candidates": [{"label": "C", "score": 1.0}, {"label": "D", "score": 0.5}],
            }
            for index in range(3)
        ]
        post, diagnostics = contextualize_chords(
            primary, [chroma, chroma], [], [0, 1, 2, 3], [], 3
        )
        self.assertEqual([(item.start, item.end, item.label) for item in post], [(0, 3, "C")])
        self.assertEqual(diagnostics["corrections"][0]["reason"], "isolated one-beat excursion")

    def test_bass_context_removes_fragment_and_makes_line_monophonic(self) -> None:
        notes = [
            NoteEvent(0.0, 0.03, 36), NoteEvent(0.1, 0.8, 36, 90),
            NoteEvent(0.12, 0.7, 48, 50), NoteEvent(0.6, 1.0, 41, 90),
        ]
        post, diagnostics = contextualize_bass(notes, [TimedLabel(0, 1, "C")])
        self.assertEqual([note.midi for note in post], [36, 41])
        self.assertEqual(post[0].end, 0.6)
        self.assertEqual(len(diagnostics), 3)

    def test_regular_cymbal_stream_becomes_ride_but_downbeat_crash_remains(self) -> None:
        drums = [DrumHit(index * 0.5, "Crash", 122 if index == 0 else 91) for index in range(8)]
        post, diagnostics = classify_cymbals(
            drums, [index * 0.5 for index in range(9)], [0.0, 2.0],
            [TimedLabel(0, 4, "chorus")],
        )
        self.assertEqual(post[0].lane, "Crash")
        self.assertTrue(any(hit.lane == "Ride" for hit in post[1:]))
        self.assertTrue(diagnostics)

    def test_local_timing_report_exposes_internal_drift_with_exact_endpoints(self) -> None:
        choices = [SectionChoice("verse", "verse", 0.0, 2.0, 0, 4)]
        report = section_timing_diagnostics(choices, [0.0, 0.45, 1.0, 1.55, 2.0], 120)
        self.assertEqual(report["A"]["global_stretch_factor"], 1.0)
        self.assertEqual(report["A"]["max_absolute_residual_ms"], 50.0)

    def test_bass_fusion_recovers_rootless_seventh_chords(self) -> None:
        chords = [TimedLabel(0, 2, "C"), TimedLabel(2, 4, "F#:dim"), TimedLabel(4, 6, "C")]
        bass = [NoteEvent(0, 0.4, 45), NoteEvent(2, 2.4, 50), NoteEvent(4, 4.4, 47)]
        fused, diagnostics = fuse_chords_with_bass(chords, bass)
        self.assertEqual([item.label for item in fused], ["Am7", "D7", "C"])
        self.assertEqual(len(diagnostics), 2)

    def test_chord_stabilizer_canonicalizes_fragments_and_short_gaps(self) -> None:
        chords = [
            TimedLabel(0, 2, "C"), TimedLabel(2, 2.4, "D"),
            TimedLabel(2.4, 4, "D:7"), TimedLabel(4.1, 4.4, "A:min"),
            TimedLabel(4.4, 6.9, "Am7"),
        ]
        stable = stabilize_chords(chords, 7.0)
        self.assertEqual([item.label for item in stable], ["C", "D7", "D7", "Am7", "Am7"])
        self.assertEqual(stable[2].end, 4.1)
        self.assertEqual(stable[-1].end, 7.0)

    def test_role_mapping_and_arrangement_preserves_each_selected_section(self) -> None:
        structures = [
            TimedLabel(0, 4, "intro"), TimedLabel(4, 12, "verse"),
            TimedLabel(12, 20, "pre-chorus"), TimedLabel(20, 28, "chorus"),
            TimedLabel(28, 32, "outro"),
        ]
        choices = [
            SectionChoice("bookend", "intro", 0, 4, 0, 8),
            SectionChoice("verse", "verse", 4, 12, 8, 16),
            SectionChoice("chorus", "chorus", 20, 28, 40, 16),
        ]
        self.assertEqual(structure_role("pre-chorus"), "verse")
        self.assertEqual(arrangement_steps(structures, choices), [
            {"bank": 0, "repeats": 1},
            {"bank": 1, "repeats": 1},
            {"bank": 2, "repeats": 1},
        ])

    def test_structure_selection_keeps_repeated_source_labels_as_distinct_sections(self) -> None:
        beats = [index * 0.5 for index in range(21)]
        structures = [
            TimedLabel(0, 2, "intro"), TimedLabel(2, 4, "verse"),
            TimedLabel(4, 6, "verse"), TimedLabel(6, 8, "chorus"),
            TimedLabel(8, 10, "outro"),
        ]
        analysis = Analysis(
            beats=beats, downbeats=[0, 2, 4, 6, 8, 10], bpm=120,
            beats_per_bar=4, structures=structures, chords=[], drums=[], bass=[],
        )
        choices = choose_sections(analysis, 10, "auto")
        self.assertEqual(
            [choice.source_label for choice in choices],
            ["intro", "verse", "verse", "chorus", "outro"],
        )
        self.assertEqual(
            arrangement_steps(structures, choices),
            [{"bank": index, "repeats": 1} for index in range(5)],
        )

    def test_structure_section_uses_real_complete_downbeat_boundaries(self) -> None:
        beats = [0.10 + index * 0.505 for index in range(13)]
        downbeats = [beats[index] for index in (0, 4, 8, 12)]
        analysis = Analysis(
            beats=beats, downbeats=downbeats, bpm=119, beats_per_bar=4,
            structures=[TimedLabel(0.3, 4.4, "verse")], chords=[], drums=[], bass=[],
        )
        choices = choose_sections(analysis, 6.2, "auto")
        self.assertEqual(len(choices), 1)
        self.assertEqual(choices[0].start, downbeats[0])
        self.assertEqual(choices[0].end, downbeats[2])
        self.assertEqual(choices[0].beats, 8)

    def test_chord_normalization(self) -> None:
        self.assertEqual(normalize_chord("C:maj7"), ("Cmaj7", False))
        self.assertEqual(normalize_chord("Bb:min7/Db"), ("Bbm7/Db", False))
        self.assertEqual(normalize_chord("N")[0], "-")
        chord, approximated = normalize_chord("F:weird")
        self.assertEqual(chord, "F")
        self.assertTrue(approximated)

    def test_drum_quantization_preserves_native_lane_order(self) -> None:
        section = SectionChoice("verse", "verse", 0, 2, 0, 4)
        patterns, residuals = quantize_drums(
            [DrumHit(0.0, "Kick", 38), DrumHit(0.26, "Snare", 122)],
            section, [0, 0.5, 1, 1.5, 2], 120, 4,
        )
        self.assertEqual(len(patterns), 4)
        self.assertIn("g", patterns[0]["lanes"][0])
        self.assertIn("a", patterns[0]["lanes"][1])
        self.assertEqual(len(patterns[0]["lanes"]), 10)
        self.assertEqual(len(residuals), 2)

    def test_musical_quantization_tracks_irregular_source_beats(self) -> None:
        section = SectionChoice("verse", "verse", 0.0, 2.08, 0, 4)
        patterns, _, diagnostics = quantize_musical(
            [TimedLabel(0.0, 1.03, "C"), TimedLabel(1.03, 2.08, "F")],
            [NoteEvent(0.51, 0.78, 36), NoteEvent(1.57, 1.83, 41)],
            section, [0.0, 0.51, 1.03, 1.57, 2.08], 120.0,
        )
        chord_steps = [step for pattern in patterns for step in pattern["chords"]]
        bass_steps = [step for pattern in patterns for step in pattern["bass"]]
        self.assertEqual(
            [(index, step["value"]) for index, step in enumerate(chord_steps) if step["state"] == "onset"],
            [(0, "C"), (8, "F")],
        )
        self.assertEqual(
            [(index, step["value"]) for index, step in enumerate(bass_steps) if step["state"] == "onset"],
            [(4, "C2"), (12, "F2")],
        )
        self.assertTrue(any(item["kind"] == "bass" for item in diagnostics))

    def test_repair_requires_repeated_low_threshold_evidence(self) -> None:
        detected = [
            DrumHit(0.0, "Snare", 91, 0.9),
            DrumHit(2.0, "Snare", 91, 0.9),
        ]
        candidates = [DrumHit(4.0, "Snare", 91, 0.08, 0.7, "candidate")]
        repaired, diagnostics = repair_drum_hits(
            detected, candidates, [0.0], 120, 4, 4, {"Snare": 0.16}, 2, 8, True
        )
        self.assertEqual(len(repaired), 3)
        self.assertEqual(repaired[-1].provenance, "repaired_repetition")
        self.assertTrue(diagnostics[0]["accepted"])
        self.assertEqual(drum_state(repaired[-1].velocity), "x")

    def test_four_four_snare_dynamics_use_rhythmic_position(self) -> None:
        hits = [
            DrumHit(0.5, "Snare", 91),
            DrumHit(0.75, "Snare", 122),
            DrumHit(1.5, "Snare", 38),
            DrumHit(0.0, "Kick", 91),
            DrumHit(1.0, "Mid Tom", 91, provenance="repaired_transient"),
            DrumHit(1.0, "Closed HH", 91),
        ]
        shaped, diagnostics = shape_drum_dynamics(hits, [0.0], 120.0, 4, 4)
        self.assertEqual(
            [drum_state(hit.velocity) for hit in shaped], ["a", "g", "a", "x", "x"]
        )
        self.assertEqual(len(diagnostics), 4)
        self.assertEqual(diagnostics[-1]["output_state"], "suppressed")

        shifted, _ = shape_drum_dynamics(
            [DrumHit(1.0, "Snare", 91)],
            [0.0, 0.5, 1.0, 1.5, 2.0], 120.0, 4, 4, [0.5],
        )
        self.assertEqual(drum_state(shifted[0].velocity), "a")

    def test_rim_mode_keeps_only_the_dominant_kick_pattern(self) -> None:
        detected = []
        for bar in range(4):
            detected.append(DrumHit(bar * 1.0, "Kick", 91, 0.8))
            if bar < 3:
                detected.extend([
                    DrumHit(bar * 1.0 + 0.75, "Kick", 91, 0.6),
                    DrumHit(bar * 1.0 + 0.75, "Cross-stick / Rim", 91, 0.3),
                    DrumHit(bar * 1.0 + 0.75, "Closed HH", 91, 0.2),
                ])
        normalized, diagnostics = repair_drum_hits(
            detected, [], [0.0], 120, 2, 4, {}, 3, 8, True, True
        )
        self.assertEqual(sum(hit.lane == "Kick" for hit in normalized), 4)
        self.assertTrue(all(
            drum_state(hit.velocity) == "a" for hit in normalized if hit.lane == "Kick"
        ))
        self.assertEqual(sum(hit.lane == "Cross-stick / Rim" for hit in normalized), 3)
        self.assertFalse(any(hit.lane == "Closed HH" for hit in normalized))
        self.assertEqual(diagnostics[-1]["stable_kick_positions"], [0])


class JamJarTests(unittest.TestCase):
    def test_export_is_copy_ready_and_uses_imported_only(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stems: dict[str, Path] = {}
            for stem in ("drums", "bass", "other", "vocals"):
                path = root / f"{stem}.wav"
                write_silence(path, seconds=2.01)
                stems[stem] = path
            analysis = Analysis(
                beats=[0, 0.5, 1, 1.5, 2], downbeats=[0, 2], bpm=120,
                beats_per_bar=4,
                structures=[TimedLabel(0, 2, "verse")],
                chords=[TimedLabel(0, 1, "C:maj"), TimedLabel(1, 2, "F:maj7")],
                drums=[DrumHit(0, "Kick"), DrumHit(0.5, "Snare")],
                bass=[NoteEvent(0, 0.75, 36)],
            )
            choice = SectionChoice("verse", "verse", 0, 2.01, 0, 4)
            choices = [choice] * 5
            song, manifest = export_song(
                staging_root=root / "staging", display_name="POC Song", source_hash="a" * 64,
                stem_paths=stems, analysis=analysis, choices=choices,
                arrangement=[{"bank": index, "repeats": 1} for index in range(5)],
                arrangement_loop=True,
            )
            self.assertEqual({item.name for item in song.iterdir()}, {"POC_Song.jamjar", "imported"})
            self.assertFalse((song / "generated").exists())
            document = json.loads((song / "POC_Song.jamjar").read_text(encoding="utf-8"))
            self.assertEqual(document["beat_lane_schema"], 3)
            self.assertEqual(len(document["sections"]), 5)
            self.assertEqual(len(document["looper"]["banks"]), 5)
            self.assertEqual(len(document["looper"]["banks"][0]["lanes"]), 4)
            self.assertTrue(document["looper"]["arrangement"]["enabled"])
            for lane in document["looper"]["banks"][0]["lanes"]:
                self.assertTrue(lane["asset_path"].startswith("imported/"))
                self.assertEqual(lane["origin_kind"], "imported")
                self.assertFalse(lane["local_only"])
                self.assertFalse(lane["loop_enabled"])
                self.assertEqual(lane["sample_rate"], 48000)
                self.assertEqual(int(lane["source_frames"]), 96000)
            self.assertEqual(document["track"]["file_path"], "")
            self.assertEqual(len(manifest["assets"]), 20)
            self.assertEqual(manifest["quantization"]["A"]["source_drift_ms"], 10.0)
            self.assertEqual(manifest["quantization"]["A"]["drift_ms"], 0.0)

    def test_single_choice_is_padded_with_valid_empty_sections(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stems = {}
            for stem in ("drums", "bass", "other", "vocals"):
                path = root / f"{stem}.wav"
                write_silence(path)
                stems[stem] = path
            analysis = Analysis(
                beats=[0, 0.5, 1, 1.5, 2], downbeats=[0, 2], bpm=120,
                beats_per_bar=4, structures=[], chords=[], drums=[], bass=[],
            )
            song, _ = export_song(
                staging_root=root / "staging", display_name="One Section",
                source_hash="b" * 64, stem_paths=stems, analysis=analysis,
                choices=[SectionChoice("full", "Full Sample", 0, 2, 0, 4)],
                arrangement=[{"bank": 0, "repeats": 1}], arrangement_loop=True,
            )
            document = json.loads((song / "One_Section.jamjar").read_text(encoding="utf-8"))
            self.assertEqual(len(document["sections"]), 4)
            self.assertEqual(document["sections"][1]["label"], "B")
            self.assertEqual(document["sections"][1]["name"], "Unused B")


if __name__ == "__main__":
    unittest.main()
