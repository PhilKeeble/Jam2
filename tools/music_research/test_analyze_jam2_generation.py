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
        self.assertEqual(
            chord_details("C6")["pitch_classes"],
            {0, 4, 7, 9},
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

    def test_inversion_must_improve_the_surrounding_bass_route(
        self,
    ) -> None:
        recipe = sample("inversion-route", 0)["recipe"]
        recipe["progression"]["final_chord_plan"] = [
            "1:1 C",
            "2:1 G/B",
            "3:1 Am",
        ]
        recipe["progression"]["theory_decisions"] = [{
            "kind": "inversion",
            "beat": 4,
            "before": "G",
            "after": "G/B",
            "analysis": "First inversion",
            "resolution_target": "G/B",
        }]
        recipe["roles"]["bass_events"].append({
            "tick": 4 * 12,
            "duration_ticks": 12,
            "midi": 47,
            "role": "bass",
            "relationship": "stepwise bass route",
            "articulation": "connected",
        })
        self.assertEqual(
            theory_operation_audit(recipe)[0]["failures"],
            [],
        )

        worse = copy.deepcopy(recipe)
        worse["progression"]["final_chord_plan"][1] = "2:1 G/D"
        worse["progression"]["theory_decisions"][0][
            "after"
        ] = "G/D"
        worse["progression"]["theory_decisions"][0][
            "resolution_target"
        ] = "G/D"
        worse["roles"]["bass_events"][-1]["midi"] = 38
        self.assertIn(
            "inversion does not improve the surrounding bass route",
            theory_operation_audit(worse)[0]["failures"],
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

    def test_contemporary_country_contract_preserves_return_and_response(
        self,
    ) -> None:
        country = sample("country-contemporary", 0, complexity=4)
        country["style_id"] = "country"
        country["profile_id"] = "country_contemporary"
        recipe = country["recipe"]
        recipe["mode"] = "Major"
        recipe["progression"]["id"] = "country-145"
        recipe["progression"]["theory_decisions"] = []
        recipe["groove"]["id"] = "country-rock"
        recipe["native_form"]["sections"][1]["label"] = "B"
        recipe["native_form"]["sections"][1]["role"] = (
            "major arrival"
        )
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
            "tick": 3 * 4 * 12 + 3 * 12,
            "duration_ticks": 6,
            "midi": 67,
            "role": "countermelody",
            "relationship": (
                "A short answer enters at the end of the four-bar "
                "Country phrase."
            ),
        }]
        self.assertEqual(contract_failures(country), [])

        missing_response = copy.deepcopy(country)
        missing_response["recipe"]["roles"]["supporting_events"] = []
        self.assertIn(
            "developed Country omits its phrase response",
            contract_failures(missing_response),
        )

        wrong_meter_groove = copy.deepcopy(country)
        wrong_meter_groove["recipe"]["groove"]["id"] = "country-waltz"
        self.assertIn(
            "meter-specific Country groove appears in an incompatible "
            "form",
            contract_failures(wrong_meter_groove),
        )

        unrelated_return = copy.deepcopy(country)
        unrelated_return["recipe"]["motif"]["events"][2]["tick"] += 18
        unrelated_return["recipe"]["motif"]["events"][3]["tick"] += 18
        self.assertIn(
            "Country return does not recall at least half of the opening "
            "two-bar sung-call rhythm",
            contract_failures(unrelated_return),
        )

        generic_colour = copy.deepcopy(country)
        generic_colour["recipe"]["progression"][
            "theory_decisions"
        ] = [{
            "kind": "country-extension",
            "beat": 4,
            "after": "Fmaj9",
            "resolution_target": "Fmaj9",
        }]
        self.assertIn(
            "Country extension uses a generic non-Country chord colour",
            contract_failures(generic_colour),
        )

        bad_connector = copy.deepcopy(country)
        bad_connector["recipe"]["roles"]["bass_events"].extend([
            {
                "tick": 3 * 12,
                "duration_ticks": 6,
                "midi": 35,
                "relationship": (
                    "A restrained connecting tone leads the Country-Pop "
                    "bass into the next section or chord."
                ),
            },
            {
                "tick": 4 * 12,
                "duration_ticks": 6,
                "midi": 43,
                "relationship": "new root",
            },
        ])
        self.assertIn(
            "Country bass connector does not resolve by step into the next "
            "written bass target",
            contract_failures(bad_connector),
        )

    def test_honky_tonk_contract_preserves_waltz_two_feel(
        self,
    ) -> None:
        country = sample("country-waltz", 0, complexity=1)
        country["style_id"] = "country"
        country["profile_id"] = "country_honky_tonk"
        country["form_id"] = "country-waltz-test"
        country["requested_meter"] = "3-4"
        recipe = country["recipe"]
        recipe["beats_per_bar"] = 3
        recipe["mode"] = "Mixolydian"
        recipe["time"]["meter_id"] = "3-4"
        recipe["native_form"]["id"] = "country-waltz-test"
        recipe["native_form"]["sections"] = [
            {
                "label": "A",
                "start_bar": 1,
                "bars": 4,
                "role": "establish",
            },
            {
                "label": "A'",
                "start_bar": 5,
                "bars": 4,
                "role": "vary",
            },
        ]
        recipe["progression"]["id"] = "country-145"
        recipe["progression"]["theory_decisions"] = []
        recipe["progression"]["base_harmony"] = [
            {
                "beat": bar * 3,
                "duration_beats": 3,
                "roman": "I",
                "chord": "C",
            }
            for bar in range(8)
        ]
        recipe["motif"]["events"] = [
            {"tick": 0, "duration_ticks": 12, "midi": 60},
            {"tick": 12, "duration_ticks": 12, "midi": 62},
            {"tick": 24, "duration_ticks": 12, "midi": 70},
            {"tick": 4 * 3 * 12, "duration_ticks": 12, "midi": 64},
            {
                "tick": 4 * 3 * 12 + 12,
                "duration_ticks": 12,
                "midi": 65,
            },
            {
                "tick": 4 * 3 * 12 + 24,
                "duration_ticks": 12,
                "midi": 67,
            },
        ]
        recipe["roles"]["bass_events"] = [
            {
                "tick": (bar * 3 + beat) * 12,
                "duration_ticks": 6,
                "midi": 36 if beat == 0 else 43,
            }
            for bar in range(8)
            for beat in (0, 2)
        ]
        recipe["roles"]["supporting_events"] = []
        recipe["groove"]["id"] = "country-waltz"
        recipe["groove"]["phrase_plan"] = [
            {
                "start_bar": 1,
                "end_bar": 4,
                "fill_id": "country-waltz-1",
            },
            {
                "start_bar": 5,
                "end_bar": 8,
                "fill_id": "country-waltz-2",
            },
        ]
        recipe["groove"]["performance_events"] = [
            {
                "tick": 12,
                "lane": "cross_stick",
                "velocity": 72,
                "fill": False,
            }
        ]
        self.assertEqual(contract_failures(country), [])

        doubled_backbeat = copy.deepcopy(country)
        doubled_backbeat["recipe"]["groove"][
            "performance_events"
        ].append({
            "tick": 12,
            "lane": "snare",
            "velocity": 76,
            "fill": False,
        })
        self.assertIn(
            "Country waltz doubles snare and cross-stick on the same pulse",
            contract_failures(doubled_backbeat),
        )

        wrong_fill = copy.deepcopy(country)
        wrong_fill["recipe"]["groove"]["phrase_plan"][0][
            "fill_id"
        ] = "country-train-1"
        self.assertIn(
            "meter-specific Country form uses the wrong fill vocabulary",
            contract_failures(wrong_fill),
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

    def test_rnb_soul_contracts_preserve_profile_space_and_recall(
        self,
    ) -> None:
        classic = sample(
            "classic-soul",
            0,
            complexity=1,
        )
        classic["style_id"] = "rnb-soul"
        classic["profile_id"] = "soul_classic_motown"
        classic_recipe = classic["recipe"]
        classic_recipe["mode"] = "Major"
        classic_recipe["progression"]["id"] = "rnb-1625"
        classic_recipe["progression"]["theory_decisions"] = []
        classic_recipe["groove"]["id"] = "rnb-motown"
        classic_recipe["roles"]["bass_events"] = [
            {
                "tick": tick,
                "duration_ticks": 6,
                "midi": 36 + (beat % 4),
                "role": "bass",
                "relationship": "melodic Soul pulse",
            }
            for beat, tick in enumerate(range(0, 384, 12))
        ]
        classic_recipe["roles"]["supporting_events"] = [{
            "tick": 180,
            "duration_ticks": 6,
            "midi": 55,
            "role": "call_response",
            "relationship": "four-bar band response",
        }]
        self.assertEqual(contract_failures(classic), [])

        neo = sample(
            "neo-soul",
            0,
            complexity=4,
        )
        neo["style_id"] = "rnb-soul"
        neo["profile_id"] = "rnb_contemporary_neosoul"
        neo_recipe = neo["recipe"]
        neo_recipe["mode"] = "Major"
        neo_recipe["progression"]["id"] = "rnb-1625"
        neo_recipe["progression"]["theory_decisions"] = []
        neo_recipe["groove"]["id"] = "rnb-laid-back"
        neo_recipe["motif"]["events"].extend([
            {
                "tick": 192,
                "duration_ticks": 12,
                "midi": 64,
                "melodic_role": "recalled call",
                "chord_role": "third",
            },
            {
                "tick": 204,
                "duration_ticks": 12,
                "midi": 65,
                "melodic_role": "recalled call",
                "chord_role": "colour",
            },
        ])
        neo_recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 48,
                "duration_ticks": 12,
                "midi": 36,
                "role": "bass",
                "relationship": "long root",
            }
            for bar in range(8)
        ]
        neo_recipe["roles"]["bass_events"].append({
            "tick": 90,
            "duration_ticks": 6,
            "midi": 35,
            "role": "bass",
            "relationship": "A sparse off-beat semitone pickup",
        })
        self.assertEqual(contract_failures(neo), [])

        crowded = copy.deepcopy(neo)
        crowded["recipe"]["roles"]["supporting_events"] = [
            {
                "tick": tick,
                "duration_ticks": 6,
                "midi": 55,
                "role": "lead_harmony",
                "relationship": "constant double",
            }
            for tick in range(0, 384, 6)
        ]
        self.assertIn(
            "R&B/Soul ensemble responses have become a continuous second "
            "lead",
            contract_failures(crowded),
        )

    def test_funk_contract_preserves_vamp_interlock_and_bounded_answers(
        self,
    ) -> None:
        developed = sample("funk-developed", 0, complexity=4)
        developed["style_id"] = "funk"
        developed["profile_id"] = "funk_static_pocket"
        recipe = developed["recipe"]
        recipe["mode"] = "Mixolydian"
        recipe["progression"]["id"] = "funk-static-1"
        recipe["progression"]["theory_decisions"] = []
        recipe["groove"]["id"] = "funk-the-one"
        recipe["native_form"]["sections"][0]["label"] = "Pocket A"
        recipe["native_form"]["sections"][1]["label"] = "Break B"
        recipe["motif"]["events"] = [
            {
                "tick": cycle + offset,
                "duration_ticks": 3,
                "midi": 60 + (offset // 12),
                "melodic_role": "recalled two-bar attack cell",
                "chord_role": "root",
            }
            for cycle in range(0, 384, 96)
            for offset in (0, 12, 36)
        ]
        recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 3,
                "midi": 36 if offset in (0, 24) else 31,
                "role": "bass",
                "relationship": (
                    "One chromatic pickup resolves on the one"
                    if bar == 3 and offset == 45
                    else "syncopated root/fifth cell"
                ),
            }
            for bar in range(8)
            for offset in (0, 21, 24, 45)
            if not (bar == 4 and offset == 0)
        ]
        recipe["roles"]["supporting_events"] = [
            {
                "tick": 174,
                "duration_ticks": 3,
                "midi": 60,
                "role": "horn_stab",
                "relationship": "short response in the riff breath",
            },
            {
                "tick": 186,
                "duration_ticks": 3,
                "midi": 66,
                "role": "horn_stab",
                "relationship": "short response in the riff breath",
            },
        ]
        recipe["groove"]["performance_events"] = [
            {
                "tick": bar * 48,
                "lane": "kick",
                "velocity": 112,
                "offset_ms": 0,
                "fill": False,
            }
            for bar in range(8)
        ]
        self.assertEqual(contract_failures(developed), [])

        foundation = copy.deepcopy(developed)
        foundation["recipe"]["complexity"] = 1
        foundation["requested_complexity"] = 1
        foundation["recipe"]["roles"]["supporting_events"] = []
        for event in foundation["recipe"]["roles"]["bass_events"]:
            event["relationship"] = "syncopated root/fifth cell"
        self.assertEqual(contract_failures(foundation), [])

        overdeveloped = copy.deepcopy(developed)
        overdeveloped["recipe"]["progression"]["theory_decisions"] = [{
            "kind": "secondary-dominant",
        }]
        self.assertIn(
            "Funk complexity adds chord operations instead of developing "
            "the interlock",
            contract_failures(overdeveloped),
        )

    def test_hiphop_trap_contracts_preserve_beat_phrase_and_808_form(
        self,
    ) -> None:
        boom_bap = sample("boom-bap-developed", 0, complexity=4)
        boom_bap["style_id"] = "hiphop-trap"
        boom_bap["profile_id"] = "hiphop_boom_bap"
        boom_bap["form_id"] = "boombap-12"
        boom_bap["requested_bars"] = 12
        boom_recipe = boom_bap["recipe"]
        boom_recipe["bars"] = 12
        boom_recipe["mode"] = "Natural Minor"
        boom_recipe["native_form"]["id"] = "boombap-12"
        boom_recipe["native_form"]["sections"] = [
            {
                "label": "Loop A",
                "start_bar": 1,
                "bars": 4,
                "role": "establish",
                "relationship": "core",
            },
            {
                "label": "Bass / Recut A'",
                "start_bar": 5,
                "bars": 4,
                "role": "activate bass",
                "relationship": "one recut",
            },
            {
                "label": "Hook / Cut / Return",
                "start_bar": 9,
                "bars": 4,
                "role": "return",
                "relationship": "bounded answer",
            },
        ]
        boom_recipe["progression"]["id"] = "boombap-static-minor"
        boom_recipe["progression"]["theory_decisions"] = []
        boom_recipe["groove"]["id"] = "hiphop-boom-bap"
        boom_recipe["motif"]["events"] = [
            {
                "tick": cycle + offset,
                "duration_ticks": 3,
                "midi": 60,
                "melodic_role": "sample-like beat phrase",
                "chord_role": "root",
            }
            for cycle in range(0, 576, 192)
            for offset in (0, 36, 111)
        ]
        boom_recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 3,
                "midi": 36 if offset == 0 else 43,
                "role": "bass",
                "relationship": (
                    "One chromatic bass pickup leads back into the loop"
                    if bar == 3 and offset == 36
                    else "short root/answer motif"
                ),
                "articulation": "short-sample-like",
            }
            for bar in range(12)
            for offset in (0, 36)
        ]
        boom_recipe["roles"]["supporting_events"] = [{
            "tick": 381,
            "duration_ticks": 3,
            "midi": 60,
            "role": "riff",
            "relationship": "one-shot hook answer",
        }]
        boom_recipe["groove"]["performance_events"] = [
            {
                "tick": bar * 48 + offset,
                "lane": "snare",
                "velocity": 108,
                "offset_ms": 5,
                "fill": False,
            }
            for bar in range(12)
            for offset in (12, 36)
        ]
        self.assertEqual(contract_failures(boom_bap), [])

        trap = copy.deepcopy(boom_bap)
        trap["id"] = "trap-developed"
        trap["profile_id"] = "hiphop_trap"
        trap["form_id"] = "trap-12"
        trap_recipe = trap["recipe"]
        trap_recipe["native_form"]["id"] = "trap-12"
        trap_recipe["native_form"]["sections"] = [
            {
                "label": "Core A",
                "start_bar": 1,
                "bars": 4,
                "role": "establish half-time frame",
                "relationship": "core",
            },
            {
                "label": "Roll / Slide A'",
                "start_bar": 5,
                "bars": 4,
                "role": "activate one roll and 808 approach",
                "relationship": "bounded activation",
            },
            {
                "label": "Negative Space / Beat Switch B",
                "start_bar": 9,
                "bars": 4,
                "role": "subtract hats and reinterpret the beat",
                "relationship": "negative space",
            },
        ]
        trap_recipe["progression"]["id"] = "trap-static-minor"
        trap_recipe["groove"]["id"] = "trap-sparse"
        trap_recipe["motif"]["events"] = copy.deepcopy(
            boom_recipe["motif"]["events"]
        )
        trap_bass = []
        for bar in range(12):
            trap_bass.append({
                "tick": bar * 48,
                "duration_ticks": 30,
                "midi": 36,
                "role": "bass",
                "relationship": "long tuned 808 root",
                "articulation": "808-sustain",
            })
            if bar % 4 == 2:
                trap_bass.append({
                    "tick": bar * 48 + 24,
                    "duration_ticks": 18,
                    "midi": 36,
                    "role": "bass",
                    "relationship": "single octave retrigger",
                    "articulation": "808-sustain",
                })
            if bar % 4 == 3:
                trap_bass.append({
                    "tick": bar * 48 + 45,
                    "duration_ticks": 3,
                    "midi": 35,
                    "role": "bass",
                    "relationship": "bounded semitone 808 approach",
                    "articulation": "808-slide",
                })
        trap_recipe["roles"]["bass_events"] = trap_bass
        trap_recipe["roles"]["supporting_events"] = [{
            "tick": 381,
            "duration_ticks": 3,
            "midi": 72,
            "role": "riff",
            "relationship": "single bell answer",
        }]
        trap_drums = [
            {
                "tick": bar * 48 + 24,
                "lane": "snare",
                "velocity": 112,
                "offset_ms": 0,
                "fill": False,
            }
            for bar in range(12)
        ]
        trap_drums.extend({
            "tick": 372 + offset,
            "lane": "closed_hat",
            "velocity": 82,
            "offset_ms": 0,
            "fill": False,
        } for offset in (0, 2, 6, 8, 10))
        trap_drums.append({
            "tick": 394,
            "lane": "kick",
            "velocity": 110,
            "offset_ms": 0,
            "fill": False,
        })
        trap_recipe["groove"]["performance_events"] = trap_drums
        self.assertEqual(contract_failures(trap), [])

        unshaped = copy.deepcopy(trap)
        unshaped["recipe"]["groove"]["performance_events"].append({
            "tick": 384,
            "lane": "closed_hat",
            "velocity": 80,
            "offset_ms": 0,
            "fill": False,
        })
        self.assertIn(
            "Trap negative-space state does not remove hats for its opening "
            "two beats",
            contract_failures(unshaped),
        )

    def test_electronic_foundation_contracts_preserve_distinct_cells(
        self,
    ) -> None:
        def electronic_foundation(
            profile_id: str,
            progression_id: str,
            groove_id: str,
            mode: str,
        ) -> dict:
            generated = sample(profile_id, 0, complexity=1)
            generated["style_id"] = "electronic"
            generated["profile_id"] = profile_id
            recipe = generated["recipe"]
            recipe["mode"] = mode
            recipe["native_form"]["phrase_bars"] = 4
            recipe["progression"]["id"] = progression_id
            recipe["progression"]["theory_decisions"] = []
            recipe["groove"]["id"] = groove_id
            recipe["roles"]["supporting_events"] = []
            return generated

        house = electronic_foundation(
            "electronic_house",
            "edm-1564",
            "edm-house",
            "Major",
        )
        house_recipe = house["recipe"]
        house_recipe["motif"]["events"] = [
            {
                "tick": tick,
                "duration_ticks": 6,
                "midi": 60,
                "melodic_role": "two-bar house hook",
                "chord_role": "root",
            }
            for tick in range(0, 384, 96)
        ]
        house_recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 6,
                "midi": 36,
                "role": "bass",
                "relationship": "offbeat",
            }
            for bar in range(8)
            for offset in (6, 30)
        ]
        house_recipe["groove"]["performance_events"] = [
            {
                "tick": tick,
                "lane": "kick",
                "velocity": 110,
                "offset_ms": 0,
                "fill": False,
            }
            for tick in range(0, 384, 12)
        ]
        self.assertEqual(contract_failures(house), [])

        techno = electronic_foundation(
            "electronic_techno",
            "techno-single-centre",
            "edm-techno",
            "Natural Minor",
        )
        techno_recipe = techno["recipe"]
        techno_recipe["motif"]["events"] = [
            {
                "tick": tick,
                "duration_ticks": 6,
                "midi": 60,
                "melodic_role": "three-beat process cell",
                "chord_role": "root",
            }
            for tick in range(0, 384, 36)
        ]
        techno_recipe["roles"]["bass_events"] = [
            {
                "tick": tick,
                "duration_ticks": 6,
                "midi": 36,
                "role": "bass",
                "relationship": "quarter pulse",
            }
            for tick in range(0, 384, 12)
        ]
        techno_recipe["groove"]["performance_events"] = [
            {
                "tick": tick,
                "lane": "kick",
                "velocity": 112,
                "offset_ms": 0,
                "fill": False,
            }
            for tick in range(0, 384, 12)
        ]
        self.assertEqual(contract_failures(techno), [])

        breakbeat = electronic_foundation(
            "electronic_breakbeat",
            "edm-minor-loop",
            "edm-breakbeat",
            "Natural Minor",
        )
        breakbeat_recipe = breakbeat["recipe"]
        breakbeat_recipe["motif"]["events"] = copy.deepcopy(
            house_recipe["motif"]["events"]
        )
        breakbeat_recipe["roles"]["bass_events"] = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 6,
                "midi": 36,
                "role": "bass",
                "relationship": "broken answer",
            }
            for bar in range(8)
            for offset in (0, 18, 24, 42)
        ]
        breakbeat_recipe["groove"]["performance_events"] = [
            {
                "tick": bar * 48 + offset,
                "lane": "kick",
                "velocity": 108,
                "offset_ms": 2,
                "fill": False,
            }
            for bar in range(8)
            for offset in (0, 18, 30)
        ]
        self.assertEqual(contract_failures(breakbeat), [])

        overpitched = copy.deepcopy(techno)
        overpitched["recipe"]["motif"]["events"][1]["midi"] = 64
        self.assertIn(
            "Techno pitch cell exceeds its centre-plus-neighbour contract",
            contract_failures(overpitched),
        )

    def test_roots_reggae_contract_preserves_riddim_interlock_and_form(
        self,
    ) -> None:
        reggae = sample("reggae-developed", 0, complexity=8)
        reggae["style_id"] = "reggae"
        reggae["profile_id"] = "reggae_roots"
        reggae["form_id"] = "reggae-steppers-12"
        reggae["requested_bars"] = 12
        reggae["research_constraints"] = {
            "minimum_bpm": 65,
            "maximum_bpm": 100,
            "progression_families": ["reggae_bass_led"],
        }
        recipe = reggae["recipe"]
        recipe["bars"] = 12
        recipe["bpm"] = 82
        recipe["mode"] = "Major"
        recipe["native_form"]["id"] = "reggae-steppers-12"
        recipe["native_form"]["sections"] = [
            {
                "label": "Steppers A",
                "start_bar": 1,
                "bars": 4,
                "role": "establish steppers riddim",
                "relationship": "core",
            },
            {
                "label": "Skank Subtraction A'",
                "start_bar": 5,
                "bars": 4,
                "role": "subtract upper attacks",
                "relationship": "bounded subtraction",
            },
            {
                "label": "Response / Return B",
                "start_bar": 9,
                "bars": 4,
                "role": "bounded response and return",
                "relationship": "return",
            },
        ]
        recipe["progression"] = {
            "id": "reggae-1451",
            "family_id": "reggae_bass_led",
            "theory_decisions": [],
            "base_harmony": [
                {
                    "beat": bar * 4,
                    "duration_beats": 4,
                    "roman": ("I", "IV", "V", "I")[bar % 4],
                    "chord": ("C", "F", "G", "C")[bar % 4],
                }
                for bar in range(12)
            ],
        }
        recipe["motif"]["events"] = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 3,
                "midi": 60 + (bar % 4),
                "melodic_role": "recalled vocal-like call",
                "chord_role": "profile-scale tone",
            }
            for bar in range(12)
            for offset in (0, 24)
        ]
        bass = []
        for bar in range(12):
            bass.extend([
                {
                    "tick": bar * 48,
                    "duration_ticks": 15,
                    "midi": 36,
                    "role": "bass",
                    "relationship": "low root begins the bass-led riddim",
                    "articulation": "round-sustained",
                },
                {
                    "tick": bar * 48 + 30,
                    "duration_ticks": 15,
                    "midi": 43,
                    "role": "bass",
                    "relationship": "chord-tone Roots bass answer",
                    "articulation": "round-sustained",
                },
            ])
            if bar % 4 == 3:
                bass.append({
                    "tick": bar * 48 + 42,
                    "duration_ticks": 6,
                    "midi": 35,
                    "role": "bass",
                    "relationship": "one semitone pickup into the return",
                    "articulation": "round-sustained",
                })
        recipe["roles"]["bass_events"] = bass
        support = [
            {
                "tick": bar * 48 + offset,
                "duration_ticks": 3,
                "midi": 55,
                "role": "support_comping",
                "relationship": "organ-bubble pulse complements the skank",
                "articulation": "short-offbeat",
            }
            for bar in range(11)
            for offset in (12, 36)
        ]
        support.extend([
            {
                "tick": 477,
                "duration_ticks": 3,
                "midi": 64,
                "role": "call_response",
                "relationship": "short Roots instrumental answer",
                "articulation": "short-offbeat",
            },
            {
                "tick": 573,
                "duration_ticks": 3,
                "midi": 67,
                "role": "countermelody",
                "relationship": "final dub return response",
                "articulation": "short-offbeat",
            },
        ])
        recipe["roles"]["supporting_events"] = support
        recipe["groove"] = {
            "id": "reggae-steppers",
            "phrase_plan": [
                {"start_bar": 1, "end_bar": 4, "energy": 0},
                {"start_bar": 5, "end_bar": 8, "energy": 1},
                {"start_bar": 9, "end_bar": 12, "energy": 1},
            ],
            "performance_events": [
                {
                    "tick": bar * 48 + beat * 12,
                    "lane": "kick",
                    "velocity": 92,
                    "offset_ms": 0,
                    "articulation": "controlled",
                    "role": "core",
                    "fill": False,
                }
                for bar in range(12)
                for beat in range(4)
            ],
        }
        reggae["chord_voicings"] = [
            {
                "tick": beat * 12 + 6,
                "articulation": "short-offbeat",
            }
            for beat in range(48)
        ]
        self.assertEqual(contract_failures(reggae), [])

        displaced = copy.deepcopy(reggae)
        displaced["chord_voicings"][0]["tick"] = 3
        self.assertIn(
            "Roots Reggae skank no longer attacks on the eighth-note "
            "offbeat",
            contract_failures(displaced),
        )

    def test_bossa_contract_preserves_binary_songbook_interlock(
        self,
    ) -> None:
        bossa = sample("bossa-advanced", 0, complexity=8)
        bossa["style_id"] = "bossa-nova"
        bossa["profile_id"] = "bossa_songbook"
        bossa["form_id"] = "bossa-18"
        bossa["requested_bars"] = 18
        bossa["requested_meter"] = "2-4"
        bossa["research_constraints"] = {
            "minimum_bpm": 70,
            "maximum_bpm": 155,
            "progression_families": ["bossa_songbook_functional"],
        }
        recipe = bossa["recipe"]
        recipe["bars"] = 18
        recipe["beats_per_bar"] = 2
        recipe["bpm"] = 118
        recipe["mode"] = "Major"
        recipe["time"]["meter_id"] = "2-4"
        recipe["native_form"]["id"] = "bossa-18"
        recipe["native_form"]["sections"] = [
            {
                "label": "Call A",
                "start_bar": 1,
                "bars": 5,
                "role": "five-bar theme statement",
                "relationship": "core",
            },
            {
                "label": "Call A'",
                "start_bar": 6,
                "bars": 5,
                "role": "five-bar related answer",
                "relationship": "recall",
            },
            {
                "label": "Turn B",
                "start_bar": 11,
                "bars": 4,
                "role": "four-bar contrasting turn",
                "relationship": "contrast",
            },
            {
                "label": "Return / Tag",
                "start_bar": 15,
                "bars": 4,
                "role": "four-bar return and soft tag",
                "relationship": "return",
            },
        ]
        recipe["progression"] = {
            "id": "bossa-2516",
            "family_id": "bossa_songbook_functional",
            "theory_decisions": [],
            "base_harmony": [
                {
                    "beat": bar * 2,
                    "duration_beats": 2,
                    "roman": ("ii7", "V7", "Imaj7", "vi7")[bar % 4],
                    "chord": ("Dm7", "G7", "Cmaj7", "Am7")[bar % 4],
                }
                for bar in range(18)
            ],
        }
        recipe["motif"]["events"] = [
            {
                "tick": bar * 24 + (
                    0
                    if bar in {0, 5, 14}
                    else 12
                ),
                "duration_ticks": 6,
                "midi": 62 + bar % 5,
                "melodic_role": "recalled vocal-like call",
                "chord_role": "guide tone",
            }
            for bar in range(18)
        ]
        bass = []
        section_ends = {4, 9, 13}
        for bar in range(18):
            bass.extend([
                {
                    "tick": bar * 24,
                    "duration_ticks": 6,
                    "midi": 36,
                    "role": "bass",
                    "relationship": "structural root pulse",
                    "articulation": "rounded-short",
                },
                {
                    "tick": bar * 24 + 18,
                    "duration_ticks": 3 if bar in section_ends else 6,
                    "midi": 43,
                    "role": "bass",
                    "relationship": (
                        "one phrase-edge chromatic bass approach"
                        if bar in section_ends
                        else "delayed fifth answer"
                    ),
                    "articulation": "rounded-short",
                },
            ])
        recipe["roles"]["bass_events"] = bass
        recipe["roles"]["supporting_events"] = [
            {
                "tick": 5 * 24 + 21,
                "duration_ticks": 3,
                "midi": 55,
                "role": "support_comping",
                "relationship": "bounded inner guide tone",
                "articulation": "soft-detached",
            },
            {
                "tick": 10 * 24 + 21,
                "duration_ticks": 3,
                "midi": 57,
                "role": "support_comping",
                "relationship": "bounded inner guide tone",
                "articulation": "soft-detached",
            },
            {
                "tick": 17 * 24 + 21,
                "duration_ticks": 3,
                "midi": 60,
                "role": "countermelody",
                "relationship": "final lead-breath counterline",
                "articulation": "soft-detached",
            },
        ]
        recipe["groove"] = {
            "id": "bossa-core",
            "phrase_plan": [
                {"start_bar": 1, "end_bar": 5, "energy": 0},
                {"start_bar": 6, "end_bar": 10, "energy": 0},
                {"start_bar": 11, "end_bar": 14, "energy": 1},
                {"start_bar": 15, "end_bar": 18, "energy": 0},
            ],
            "performance_events": [],
        }
        bossa["chord_voicings"] = [
            {
                "tick": bar * 24 + (6 if bar % 2 == 0 else 15),
                "articulation": "soft-detached",
            }
            for bar in range(18)
        ]
        self.assertEqual(contract_failures(bossa), [])

        collapsed = copy.deepcopy(bossa)
        collapsed["chord_voicings"][0]["tick"] = 0
        self.assertIn(
            "Bossa upper voicing has collapsed onto the bass pulse",
            contract_failures(collapsed),
        )

    def test_modern_metal_contract_preserves_heavy_clean_riff_form(
        self,
    ) -> None:
        metal = sample("metal-advanced", 0, complexity=8)
        metal["style_id"] = "metal-experimental"
        metal["profile_id"] = "metal_modern_progressive"
        metal["form_id"] = "metal-contrast-18"
        metal["requested_bars"] = 18
        metal["requested_meter"] = "9-8"
        metal["research_constraints"] = {
            "minimum_bpm": 65,
            "maximum_bpm": 180,
            "progression_families": [
                "metal_articulated_riff",
                "riff_implied_harmony",
            ],
        }
        recipe = metal["recipe"]
        recipe["bars"] = 18
        recipe["beats_per_bar"] = 9
        recipe["bpm"] = 122
        recipe["mode"] = "Natural Minor"
        recipe["time"]["meter_id"] = "9-8"
        recipe["native_form"]["id"] = "metal-contrast-18"
        recipe["native_form"]["sections"] = [
            {
                "label": "Heavy A",
                "start_bar": 1,
                "bars": 6,
                "role": "six-bar grouped heavy statement",
                "relationship": "low grouped riff",
            },
            {
                "label": "Clean B",
                "start_bar": 7,
                "bars": 8,
                "role": "eight-bar clean augmented contrast",
                "relationship": "clean augmentation",
            },
            {
                "label": "Compressed Return",
                "start_bar": 15,
                "bars": 4,
                "role": "four-bar compressed heavy return",
                "relationship": "metric subtraction and return",
            },
        ]
        recipe["progression"] = {
            "id": "metal-clean-contrast",
            "family_id": "metal_articulated_riff",
            "theory_decisions": [],
            "base_harmony": [
                {
                    "beat": bar * 9,
                    "duration_beats": 9,
                    "roman": ("i", "bVIadd9", "bIII", "bVII")[bar % 4],
                    "chord": ("Am", "Fadd9", "C", "G")[bar % 4],
                }
                for bar in range(18)
            ],
        }
        melody = []
        for bar in range(18):
            offsets = (0, 72) if 6 <= bar < 14 else (0,)
            for offset in offsets:
                melody.append({
                    "tick": bar * 108 + offset,
                    "duration_ticks": 12 if 6 <= bar < 14 else 3,
                    "midi": 60 + ((bar + offset // 12) % 2) * 2,
                    "melodic_role": "clean augmented lead",
                    "chord_role": "profile tone",
                })
        recipe["motif"]["events"] = melody

        heavy_ticks = [
            bar * 108
            for bar in list(range(6)) + list(range(14, 18))
        ]
        open_ticks = [
            bar * 108
            for bar in range(6, 14)
        ]
        metal["chord_voicings"] = [
            {
                "tick": tick,
                "articulation": "palm-muted",
            }
            for tick in heavy_ticks
        ] + [
            {
                "tick": tick,
                "articulation": "open-sustain",
            }
            for tick in open_ticks
        ]
        recipe["roles"]["bass_events"] = [
            {
                "tick": tick,
                "duration_ticks": 3,
                "midi": 36,
                "role": "bass",
                "relationship": "split bass reinforces guitar attack",
                "articulation": "tight-picked",
            }
            for tick in heavy_ticks + open_ticks
        ]
        recipe["roles"]["supporting_events"] = [
            {
                "tick": bar * 108,
                "duration_ticks": 108,
                "midi": 52,
                "role": "pad",
                "relationship": "two-bar-spaced ambient clean colour",
                "articulation": "gated-choke",
            }
            for bar in (6, 8, 10, 12)
        ] + [
            {
                "tick": 14 * 108,
                "duration_ticks": 3,
                "midi": 40,
                "role": "hook_double",
                "relationship": "bounded compressed riff double",
                "articulation": "gated-choke",
            },
            {
                "tick": 14 * 108 - 3,
                "duration_ticks": 3,
                "midi": 64,
                "role": "countermelody",
                "relationship": "final clean lead-breath counterline",
                "articulation": "gated-choke",
            },
        ]
        final_extra_kick = 17 * 108 + 12
        recipe["groove"] = {
            "id": "metal-grouped",
            "phrase_plan": [
                {"start_bar": 1, "end_bar": 6, "energy": 0},
                {"start_bar": 7, "end_bar": 14, "energy": 1},
                {"start_bar": 15, "end_bar": 18, "energy": 2},
            ],
            "performance_events": [
                {
                    "tick": tick,
                    "lane": "kick",
                    "velocity": 104,
                    "offset_ms": 0,
                    "articulation": "tight",
                    "role": "core",
                    "fill": False,
                }
                for tick in heavy_ticks
            ] + [
                {
                    "tick": final_extra_kick,
                    "lane": "kick",
                    "velocity": 98,
                    "offset_ms": 0,
                    "articulation": "tight",
                    "role": "core",
                    "fill": False,
                }
            ],
        }
        self.assertEqual(contract_failures(metal), [])

        unlocked = copy.deepcopy(metal)
        unlocked["chord_voicings"][0]["tick"] = 12
        unlocked["chord_voicings"][1]["tick"] = 120
        self.assertIn(
            "Modern Metal heavy chord riff no longer locks its attacks to "
            "the authored kick pattern",
            contract_failures(unlocked),
        )


if __name__ == "__main__":
    unittest.main()
