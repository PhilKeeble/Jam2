from __future__ import annotations

import copy
import unittest

from tools.music_research.analyze_jam2_generation import (
    analyze,
    chord_details,
    contract_failures,
    drum_flow_metrics,
    full_form_evidence,
    note_pitch_class,
    theory_operation_audit,
)


def sample(
    sample_id: str,
    sample_index: int,
    complexity: int = 4,
    seed: str = "10",
) -> dict:
    recipe = {
        "bars": 8,
        "beats_per_bar": 4,
        "bpm": 110,
        "complexity": complexity,
        "mode": "Ionian",
        "tonic": "C",
        "variation": {"id": "bounded-a"},
        "time": {"meter_id": "4-4"},
        "native_form": {
            "id": "test-form",
            "sections": [
                {
                    "label": "A",
                    "start_bar": 1,
                    "bars": 4,
                    "role": "establish",
                    "relationship": "core",
                },
                {
                    "label": "A'",
                    "start_bar": 5,
                    "bars": 4,
                    "role": "answer",
                    "relationship": "bounded variation",
                },
            ],
        },
        "progression": {
            "id": "test-1451",
            "family_id": "test-family",
            "base_harmony": [
                {"beat": 0, "duration_beats": 4, "roman": "I", "chord": "C"},
                {"beat": 4, "duration_beats": 4, "roman": "IV", "chord": "F"},
            ],
        },
        "motif": {
            "cell": "1-2-3",
            "rhythm": "quarter answer",
            "events": [
                {
                    "tick": 0,
                    "duration_ticks": 12,
                    "midi": 60,
                    "melodic_role": "motif",
                    "chord_role": "root",
                },
                {
                    "tick": 12,
                    "duration_ticks": 12,
                    "midi": 62,
                    "melodic_role": "answer",
                    "chord_role": "colour",
                },
            ],
        },
        "roles": {
            "bass_events": [
                {
                    "tick": 0,
                    "duration_ticks": 12,
                    "midi": 36,
                    "role": "bass",
                    "relationship": "root",
                    "articulation": "held",
                }
            ],
            "supporting_events": [],
        },
        "groove": {
            "id": "test-pocket",
            "phrase_plan": [
                {"start_bar": 1, "end_bar": 4, "energy": 0},
                {"start_bar": 5, "end_bar": 8, "energy": 1},
            ],
            "performance_events": [
                {
                    "tick": 0,
                    "lane": "kick",
                    "velocity": 100,
                    "offset_ms": 0,
                    "articulation": "firm",
                    "role": "core",
                    "fill": False,
                },
                {
                    "tick": 12,
                    "lane": "snare",
                    "velocity": 105,
                    "offset_ms": 1,
                    "articulation": "center",
                    "role": "core",
                    "fill": False,
                },
            ],
        },
        "complexity_tools": [
            {
                "level": 1,
                "tool_id": "foundation",
                "selected": True,
            }
        ],
    }
    return {
        "id": sample_id,
        "style_id": "test-style",
        "profile_id": "test-profile",
        "profile_name": "Test Profile",
        "form_id": "test-form",
        "requested_bars": 8,
        "requested_meter": "4-4",
        "requested_complexity": complexity,
        "sample_index": sample_index,
        "seed": seed,
        "research_constraints": {
            "minimum_bpm": 80,
            "maximum_bpm": 140,
            "progression_families": ["test-family"],
        },
        "recipe": recipe,
    }


class AnalyzeJam2GenerationTests(unittest.TestCase):
    def test_extended_chord_pitch_classes_match_jam2_parser(self) -> None:
        self.assertEqual(
            chord_details("Bbm7b5")["pitch_classes"],
            {10, 1, 4, 8},
        )
        self.assertEqual(
            chord_details("A#dim7")["pitch_classes"],
            {10, 1, 4, 7},
        )
        self.assertEqual(
            chord_details("E#dim7")["pitch_classes"],
            {5, 8, 11, 2},
        )
        self.assertEqual(
            chord_details("Dmaj9")["pitch_classes"],
            {2, 6, 9, 1, 4},
        )
        self.assertEqual(
            chord_details("E9/D")["pitch_classes"],
            {4, 8, 11, 2, 6},
        )

    def test_note_parser_accepts_functional_enharmonic_spellings(self) -> None:
        self.assertEqual(note_pitch_class("E#"), 5)
        self.assertEqual(note_pitch_class("Cb"), 11)
        self.assertEqual(note_pitch_class("Fb"), 4)
        self.assertEqual(note_pitch_class("B#"), 0)

    def test_modal_contract_requires_characteristic_degree_in_melody(
        self,
    ) -> None:
        modal = sample("modal", 0)
        modal["profile_id"] = "modal_groove"
        modal["recipe"]["tonic"] = "C"
        modal["recipe"]["mode"] = "Dorian"
        self.assertIn(
            "modal melody never states its characteristic degree",
            contract_failures(modal),
        )
        modal["recipe"]["motif"]["events"].append({
            "tick": 24,
            "duration_ticks": 12,
            "midi": 69,
            "melodic_role": "Dorian characteristic degree",
            "chord_role": "collection colour",
        })
        self.assertNotIn(
            "modal melody never states its characteristic degree",
            contract_failures(modal),
        )

    def test_distinguishes_raw_and_transposition_normalized_duplicates(self) -> None:
        first = sample("first", 0)
        second = copy.deepcopy(first)
        second["id"] = "second"
        second["sample_index"] = 1
        second["seed"] = "11"

        transposed = copy.deepcopy(first)
        transposed["id"] = "transposed"
        transposed["sample_index"] = 2
        transposed["seed"] = "12"
        transposed["recipe"]["tonic"] = "D"
        for event in transposed["recipe"]["motif"]["events"]:
            event["midi"] += 2
        for event in transposed["recipe"]["roles"]["bass_events"]:
            event["midi"] += 2
        transposed["recipe"]["progression"]["base_harmony"][0]["chord"] = "D"
        transposed["recipe"]["progression"]["base_harmony"][1]["chord"] = "G"

        changed = copy.deepcopy(first)
        changed["id"] = "changed"
        changed["sample_index"] = 3
        changed["seed"] = "13"
        changed["recipe"]["motif"]["events"][1]["tick"] = 18

        audit = analyze(
            {
                "generator": "test",
                "seed_namespace": "test",
                "samples": [first, second, transposed, changed],
            }
        )
        profile = audit["profiles"]["test-profile"]
        self.assertEqual(profile["pair_count"], 6)
        self.assertEqual(profile["exact_raw_duplicate_pairs"], 1)
        self.assertEqual(profile["exact_normalized_duplicate_pairs"], 3)
        self.assertEqual(profile["contract_failures"], [])

    def test_matched_complexity_reports_nested_concepts_and_preserved_drums(self) -> None:
        low = sample("low", 0, complexity=1, seed="77")
        middle = sample("middle", 0, complexity=4, seed="77")
        high = sample("high", 0, complexity=8, seed="77")
        middle["recipe"]["complexity_tools"].append(
            {"level": 4, "tool_id": "phrase-answer", "selected": True}
        )
        high["recipe"]["complexity_tools"].extend(
            [
                {"level": 4, "tool_id": "phrase-answer", "selected": True},
                {"level": 8, "tool_id": "long-range-return", "selected": True},
            ]
        )
        audit = analyze(
            {
                "generator": "test",
                "seed_namespace": "test",
                "matched_complexity_seeds": True,
                "samples": [low, middle, high],
            }
        )
        profile = audit["profiles"]["test-profile"]
        development = profile["complexity_development"]
        self.assertEqual(development["invalid_seed_groups"], 0)
        self.assertEqual(development["lost_selected_tool_count"], 0)
        self.assertEqual(development["drum_preservation"]["changed"], 0)
        self.assertEqual(development["transition_count"], 2)

    def test_drum_flow_separates_backbone_from_bounded_detail(self) -> None:
        recipe = sample("flow", 0)["recipe"]
        recipe["bars"] = 4
        recipe["native_form"]["phrase_bars"] = 2
        events = []
        for bar in range(4):
            start = bar * 48
            events.extend([
                {
                    "tick": start,
                    "lane": "kick",
                    "role": "core",
                    "fill": False,
                },
                {
                    "tick": start + 12,
                    "lane": "snare",
                    "role": "core",
                    "fill": False,
                },
            ])
        events.append({
            "tick": 48 + 6,
            "lane": "snare",
            "role": "ghost",
            "fill": False,
        })
        events.append({
            "tick": 3 * 48 + 42,
            "lane": "mid_tom",
            "role": "fill",
            "fill": True,
        })
        recipe["groove"]["performance_events"] = events

        flow = drum_flow_metrics(recipe)
        self.assertEqual(flow["unique_backbone_bars"], 1)
        self.assertEqual(flow["unique_detailed_bars"], 3)
        self.assertEqual(flow["fill_bars"], [4])
        self.assertEqual(flow["structural_fill_ratio"], 1.0)

    def test_contract_rejects_inaudible_tom_fill_velocity(self) -> None:
        generated = sample("quiet-fill", 0)
        generated["recipe"]["groove"]["performance_events"].append({
            "tick": 42,
            "lane": "floor_tom",
            "velocity": 16,
            "role": "fill",
            "fill": True,
        })
        self.assertIn(
            "planned tom fill contains an inaudibly low ghost-band hit",
            contract_failures(generated),
        )
        generated["recipe"]["groove"][
            "performance_events"
        ][-1]["velocity"] = 48
        self.assertNotIn(
            "planned tom fill contains an inaudibly low ghost-band hit",
            contract_failures(generated),
        )

    def test_secondary_dominant_requires_resolution_and_lane_response(self) -> None:
        recipe = sample("theory", 0)["recipe"]
        recipe["progression"]["final_chord_plan"] = [
            "1:1 C",
            "4:3 D7",
            "5:1 G",
        ]
        recipe["progression"]["theory_decisions"] = [{
            "kind": "secondary-dominant",
            "beat": 14,
            "before": "C",
            "after": "D7",
            "analysis": "V/V",
            "resolution_target": "G",
        }]
        recipe["motif"]["events"].extend([
            {
                "tick": 168,
                "duration_ticks": 12,
                "midi": 62,
                "melodic_role": "defines V/V",
                "chord_role": "root",
            },
            {
                "tick": 192,
                "duration_ticks": 12,
                "midi": 67,
                "melodic_role": "resolution",
                "chord_role": "root",
            },
        ])
        recipe["roles"]["bass_events"].extend([
            {
                "tick": 168,
                "duration_ticks": 12,
                "midi": 38,
                "role": "bass",
                "relationship": "V/V root",
                "articulation": "connected",
            },
            {
                "tick": 192,
                "duration_ticks": 12,
                "midi": 43,
                "role": "bass",
                "relationship": "V root",
                "articulation": "connected",
            },
        ])

        row = theory_operation_audit(recipe)[0]
        self.assertEqual(row["failures"], [])
        self.assertTrue(row["target_at_phrase_start"])

        recipe["motif"]["events"][-2]["midi"] = 61
        melodic_conflict = theory_operation_audit(recipe)[0]
        self.assertIn(
            "sounding melody conflicts with the altered harmony",
            melodic_conflict["failures"],
        )
        recipe["motif"]["events"][-2]["midi"] = 62

        recipe["progression"]["theory_decisions"][0][
            "resolution_target"
        ] = "F"
        broken = theory_operation_audit(recipe)[0]
        self.assertIn(
            "following harmony does not match the claimed resolution",
            broken["failures"],
        )

    def test_diatonic_extension_uses_mode_collection_not_only_chord_tones(
        self,
    ) -> None:
        recipe = sample("modal-extension", 0)["recipe"]
        recipe["tonic"] = "D"
        recipe["mode"] = "Lydian"
        recipe["progression"]["final_chord_plan"] = [
            "1:1 Dmaj9",
            "5:1 E9/D",
        ]
        recipe["progression"]["theory_decisions"] = [{
            "kind": "diatonic-extension",
            "beat": 16,
            "before": "E/D",
            "after": "E9/D",
            "analysis": "Extension stacked inside Lydian",
            "resolution_target": "E9/D",
        }]
        recipe["motif"]["events"].append({
            "tick": 192,
            "duration_ticks": 12,
            "midi": 71,
            "melodic_role": "Lydian collection tone",
            "chord_role": "collection colour",
        })
        recipe["roles"]["bass_events"].append({
            "tick": 192,
            "duration_ticks": 48,
            "midi": 38,
            "role": "bass",
            "relationship": "tonic pedal",
            "articulation": "held",
        })
        recipe["roles"]["supporting_events"].append({
            "tick": 0,
            "duration_ticks": 384,
            "midi": 50,
            "role": "drone",
            "relationship": "tonic pedal",
            "articulation": "held",
        })

        row = theory_operation_audit(recipe)[0]
        self.assertEqual(row["failures"], [])

        recipe["progression"]["theory_decisions"][0]["after"] = "Eb9/D"
        recipe["progression"]["final_chord_plan"][1] = "5:1 Eb9/D"
        outside = theory_operation_audit(recipe)[0]
        self.assertIn(
            "mode-derived extension leaves the active collection",
            outside["failures"],
        )

    def test_blues_contract_preserves_identity_turnaround_and_compound_pulse(
        self,
    ) -> None:
        blues = sample("minor-blues", 0, complexity=1)
        blues["style_id"] = "blues"
        blues["profile_id"] = "blues_minor"
        blues["form_id"] = "minor-blues-12-8"
        blues["requested_bars"] = 12
        blues["requested_meter"] = "12-8"
        blues["research_constraints"]["progression_families"] = [
            "blues_native_schema"
        ]
        recipe = blues["recipe"]
        recipe["bars"] = 12
        recipe["beats_per_bar"] = 12
        recipe["mode"] = "Minor Blues"
        recipe["time"] = {
            "meter_id": "12-8",
            "tempo_pulse_units": 3,
        }
        recipe["native_form"] = {
            "id": "minor-blues-12-8",
            "sections": [
                {
                    "label": "Call A",
                    "start_bar": 1,
                    "bars": 4,
                },
                {
                    "label": "Call A'",
                    "start_bar": 5,
                    "bars": 4,
                },
                {
                    "label": "Closing Line",
                    "start_bar": 9,
                    "bars": 4,
                },
            ],
        }
        recipe["progression"] = {
            "id": "blues-minor",
            "family_id": "blues_native_schema",
            "base_harmony": [
                {
                    "beat": bar * 12,
                    "duration_beats": 12,
                    "roman": "v7" if bar == 11 else "i7",
                    "chord": "Gm7" if bar == 11 else "Cm7",
                }
                for bar in range(12)
            ],
            "theory_decisions": [],
        }
        recipe["motif"]["events"] = [
            {
                "tick": 0,
                "duration_ticks": 12,
                "midi": 63,
            },
            {
                "tick": 36,
                "duration_ticks": 12,
                "midi": 70,
            },
            {
                "tick": 4 * 144,
                "duration_ticks": 12,
                "midi": 63,
            },
            {
                "tick": 4 * 144 + 36,
                "duration_ticks": 12,
                "midi": 70,
            },
        ]
        recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 144 + pulse * 36,
                "duration_ticks": 12,
                "midi": 36,
            }
            for bar in range(12)
            for pulse in range(4)
        ]
        self.assertEqual(contract_failures(blues), [])

        unrelated_later_call = copy.deepcopy(blues)
        unrelated_later_call["recipe"]["motif"]["events"][2]["tick"] = (
            4 * 144 + 72
        )
        unrelated_later_call["recipe"]["motif"]["events"][3]["tick"] = (
            4 * 144 + 108
        )
        self.assertIn(
            "later Blues lines do not recall at least half of the opening "
            "call onset template",
            contract_failures(unrelated_later_call),
        )

        dense = copy.deepcopy(blues)
        dense["recipe"]["roles"]["bass_events"] = [
            {
                "tick": beat * 12,
                "duration_ticks": 12,
                "midi": 36,
            }
            for beat in range(12 * 12)
        ]
        dense_failures = contract_failures(dense)
        self.assertIn(
            "compound Blues bass exceeds six attacks per bar",
            dense_failures,
        )
        self.assertIn(
            "foundation compound Blues bass attacks between "
            "dotted-quarter pulse anchors",
            dense_failures,
        )

        premature = copy.deepcopy(blues)
        premature["recipe"]["progression"]["theory_decisions"] = [{
            "kind": "passing-diminished",
            "beat": 12,
        }]
        self.assertIn(
            "Blues chromatic complexity operation occurs before the "
            "closing turnaround span",
            contract_failures(premature),
        )

        functional_foundation = copy.deepcopy(blues)
        functional_foundation["recipe"]["progression"][
            "base_harmony"
        ][8]["roman"] = "iiø"
        self.assertIn(
            "foundation Minor Blues uses the later functional iiø/V7 "
            "turnaround vocabulary",
            contract_failures(functional_foundation),
        )

        advanced = copy.deepcopy(blues)
        advanced["recipe"]["complexity"] = 8
        advanced["requested_complexity"] = 8
        advanced["recipe"]["roles"]["supporting_events"] = [{
            "tick": 36,
            "duration_ticks": 12,
            "midi": 55,
        }]
        self.assertIn(
            "Blues instrumental response enters during the lead call half "
            "of a form line",
            contract_failures(advanced),
        )
        advanced["recipe"]["roles"]["supporting_events"][0]["tick"] = (
            2 * 144 + 9 * 12
        )
        self.assertNotIn(
            "Blues instrumental response enters during the lead call half "
            "of a form line",
            contract_failures(advanced),
        )

    def test_dominant_blues_contract_requires_major_minor_third_friction(
        self,
    ) -> None:
        blues = sample("dominant-blues", 0)
        blues["style_id"] = "blues"
        blues["profile_id"] = "blues_dominant"
        blues["recipe"]["progression"]["base_harmony"][-1]["roman"] = "V7"
        blues["recipe"]["motif"]["events"] = [{
            "tick": 0,
            "duration_ticks": 12,
            "midi": 63,
        }]
        self.assertIn(
            "Blues melody does not state both minor and major third",
            contract_failures(blues),
        )
        blues["recipe"]["motif"]["events"].append({
            "tick": 12,
            "duration_ticks": 12,
            "midi": 64,
        })
        self.assertNotIn(
            "Blues melody does not state both minor and major third",
            contract_failures(blues),
        )

    def test_idol_jpop_contract_requires_hook_recall_calls_and_bounded_bass(
        self,
    ) -> None:
        idol = sample("idol-jpop", 0, complexity=1)
        idol["style_id"] = "jpop-anisong"
        idol["profile_id"] = "jpop_idol_dance"
        recipe = idol["recipe"]
        recipe["mode"] = "Major"
        recipe["native_form"]["sections"][0]["label"] = "Hook A"
        recipe["native_form"]["sections"][1]["label"] = "Hook A'"
        recipe["progression"]["theory_decisions"] = []
        recipe["motif"]["events"].extend([
            {
                "tick": 4 * 4 * 12,
                "duration_ticks": 12,
                "midi": 64,
            },
            {
                "tick": 4 * 4 * 12 + 12,
                "duration_ticks": 12,
                "midi": 65,
            },
        ])
        recipe["roles"]["supporting_events"] = [{
            "tick": 3 * 4 * 12,
            "duration_ticks": 12,
            "midi": 67,
            "role": "call_response",
        }]
        self.assertEqual(contract_failures(idol), [])

        missing_call = copy.deepcopy(idol)
        missing_call["recipe"]["roles"]["supporting_events"] = []
        self.assertIn(
            "Idol J-Pop omits its foundational group-response role",
            contract_failures(missing_call),
        )

        unrelated_return = copy.deepcopy(idol)
        unrelated_return["recipe"]["motif"]["events"][2]["tick"] += 18
        unrelated_return["recipe"]["motif"]["events"][3]["tick"] += 18
        self.assertIn(
            "J-Pop return does not recall at least half of the opening "
            "two-bar onset hook",
            contract_failures(unrelated_return),
        )

        dense_bass = copy.deepcopy(idol)
        dense_bass["recipe"]["roles"]["bass_events"] = [
            {
                "tick": attack * 6,
                "duration_ticks": 6,
                "midi": 36,
            }
            for attack in range(8 * 8)
        ]
        self.assertIn(
            "J-Pop bass has reverted to a mechanical attack on every "
            "eighth note",
            contract_failures(dense_bass),
        )

    def test_anisong_key_region_is_prepared_and_returns_home(
        self,
    ) -> None:
        anisong = sample("anisong-key-region", 0, complexity=8)
        anisong["style_id"] = "jpop-anisong"
        anisong["profile_id"] = "jpop_anisong_rock"
        anisong["requested_bars"] = 12
        recipe = anisong["recipe"]
        recipe["bars"] = 12
        recipe["mode"] = "Major"
        recipe["native_form"]["sections"] = [
            {"label": "A", "start_bar": 1, "bars": 4},
            {"label": "B", "start_bar": 5, "bars": 4},
            {"label": "Return", "start_bar": 9, "bars": 4},
        ]
        recipe["progression"]["base_harmony"] = [
            {
                "beat": bar * 4,
                "duration_beats": 4,
                "roman": "I",
                "chord": "C",
            }
            for bar in range(12)
        ]
        recipe["progression"]["final_chord_plan"] = [
            "1:1 C",
            "2:1 Am",
            "3:1 F",
            "4:1 Bb7",
            "5:1 Eb",
            "6:1 Cm",
            "7:1 Ab",
            "8:1 G7",
            "9:1 C",
            "10:1 Am",
            "11:1 F",
            "12:1 C",
        ]
        recipe["progression"]["theory_decisions"] = [{
            "kind": "section-key-region",
            "beat": 16,
            "before": "C",
            "after": "Eb",
            "resolution_target": "Eb",
        }]
        recipe["motif"]["events"] = [
            {"tick": 0, "duration_ticks": 12, "midi": 60},
            {"tick": 12, "duration_ticks": 12, "midi": 62},
            {"tick": 8 * 4 * 12, "duration_ticks": 12, "midi": 64},
            {"tick": 8 * 4 * 12 + 12, "duration_ticks": 12, "midi": 65},
        ]
        recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 4 * 12,
                "duration_ticks": 12,
                "midi": 36,
            }
            for bar in range(12)
        ]
        self.assertEqual(contract_failures(anisong), [])

        unprepared = copy.deepcopy(anisong)
        unprepared["recipe"]["progression"]["final_chord_plan"][3] = (
            "4:1 F"
        )
        self.assertIn(
            "Anisong key region lacks its prepared dominant arrival",
            contract_failures(unprepared),
        )

        no_home_return = copy.deepcopy(anisong)
        no_home_return["recipe"]["progression"]["final_chord_plan"][7] = (
            "8:1 Ab"
        )
        self.assertIn(
            "Anisong key region does not prepare and return to the home key",
            contract_failures(no_home_return),
        )

    def test_full_form_evidence_accounts_for_boundaries_and_length(self) -> None:
        recipe = sample("form", 0)["recipe"]
        recipe["progression"]["final_chord_plan"] = [
            "1:1 C",
            "5:1 F",
            "8:1 C",
        ]
        recipe["motif"]["events"].extend([
            {
                "tick": 180,
                "duration_ticks": 12,
                "midi": 64,
                "melodic_role": "lead-in",
                "chord_role": "third",
            },
            {
                "tick": 192,
                "duration_ticks": 12,
                "midi": 65,
                "melodic_role": "section landing",
                "chord_role": "root",
            },
            {
                "tick": 372,
                "duration_ticks": 12,
                "midi": 60,
                "melodic_role": "cadence",
                "chord_role": "root",
            },
        ])
        recipe["roles"]["bass_events"].append({
            "tick": 192,
            "duration_ticks": 12,
            "midi": 41,
            "role": "bass",
            "relationship": "section landing",
            "articulation": "connected",
        })
        recipe["groove"]["performance_events"].extend([
            {
                "tick": 186,
                "lane": "mid_tom",
                "role": "fill",
                "fill": True,
            },
            {
                "tick": 192,
                "lane": "crash",
                "role": "section-accent",
                "fill": False,
            },
            {
                "tick": 378,
                "lane": "floor_tom",
                "role": "fill",
                "fill": True,
            },
        ])

        evidence = full_form_evidence(recipe)
        self.assertEqual(evidence["bars"], 8)
        self.assertTrue(evidence["section_coverage_valid"])
        self.assertTrue(evidence["drum_phrase_plan_coverage_valid"])
        self.assertEqual(len(evidence["boundaries"]), 1)
        self.assertEqual(evidence["boundaries"][0]["lead_in_fill_hits"], 1)
        self.assertEqual(evidence["boundaries"][0]["landing_accent_hits"], 1)
        self.assertTrue(evidence["boundaries"][0]["bass_lands_at_boundary"])
        self.assertTrue(evidence["ending"]["final_chord_root_is_tonic"])
        self.assertTrue(evidence["ending"]["final_melody_is_tonic"])
        self.assertEqual(evidence["ending"]["last_bar_fill_hits"], 1)


if __name__ == "__main__":
    unittest.main()
