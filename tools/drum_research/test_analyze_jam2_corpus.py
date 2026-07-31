from __future__ import annotations

import unittest

from tools.drum_research.analyze_jam2_corpus import metric_for_sample


def sample_with_bars(bar_hits: list[list[tuple[int, str, str]]]) -> dict:
    hits = []
    for bar, events in enumerate(bar_hits):
        for tick, lane, state in events:
            absolute = bar * 48 + tick
            hits.append(
                {
                    "tick": absolute,
                    "beat": absolute / 12,
                    "lane": lane,
                    "state": state,
                }
            )
    return {
        "id": "test",
        "profile_id": "test-profile",
        "form_id": "test-form",
        "requested_complexity": 4,
        "sample_index": 0,
        "drum_hits": hits,
        "recipe": {
            "bars": len(bar_hits),
            "beats_per_bar": 4,
            "groove": {
                "id": "test-groove",
                "performance_events": [],
                "variation_counts": {
                    "fill": 1,
                    "kick": 2,
                    "ghost": 3,
                    "cymbal": 4,
                    "advanced_cells": 0,
                },
            },
        },
    }


class AnalyzeJam2CorpusTests(unittest.TestCase):
    def test_detects_exact_two_bar_repetition(self) -> None:
        a = [(0, "Kick", "a"), (12, "Snare", "a")]
        b = [(0, "Kick", "a"), (24, "Snare", "a")]
        metric = metric_for_sample(sample_with_bars([a, b, a, b]))
        self.assertEqual(metric["uniqueCoreBarRatio"], 0.5)
        self.assertEqual(metric["period2CoreIdenticalRatio"], 1.0)
        self.assertEqual(metric["period2CoreSimilarity"], [1.0, 1.0])

    def test_cymbal_only_change_does_not_fake_core_development(self) -> None:
        core = [(0, "Kick", "a"), (12, "Snare", "a")]
        crash = core + [(0, "Crash", "a")]
        metric = metric_for_sample(sample_with_bars([crash, core]))
        self.assertEqual(metric["uniqueBarRatio"], 1.0)
        self.assertEqual(metric["uniqueCoreBarRatio"], 0.5)
        self.assertEqual(metric["consecutiveCoreIdenticalRatio"], 1.0)

    def test_real_core_change_is_counted(self) -> None:
        first = [(0, "Kick", "a"), (12, "Snare", "a")]
        second = first + [(42, "Kick", "x")]
        metric = metric_for_sample(sample_with_bars([first, second]))
        self.assertEqual(metric["uniqueCoreBarRatio"], 1.0)
        self.assertEqual(metric["adjacentCoreSymmetricChanges"], [1])
        self.assertAlmostEqual(metric["adjacentCoreSimilarity"][0], 2 / 3)

    def test_measures_exact_performance_and_three_tom_fill(self) -> None:
        sample = sample_with_bars(
            [
                [(0, "Kick", "a")],
                [
                    (36, "High Tom", "x"),
                    (39, "Mid Tom", "x"),
                    (42, "Floor Tom", "a"),
                ],
            ]
        )
        sample["recipe"]["groove"]["performance_events"] = [
            {
                "tick": 0,
                "lane": "kick",
                "velocity": 112,
                "offset_ms": -1,
                "articulation": "firm",
                "role": "core",
                "repeat_group": 0,
                "fill": False,
            },
            {
                "tick": 84,
                "lane": "high_tom",
                "velocity": 88,
                "offset_ms": 1,
                "articulation": "open",
                "role": "fill",
                "repeat_group": 0,
                "fill": True,
            },
            {
                "tick": 87,
                "lane": "mid_tom",
                "velocity": 94,
                "offset_ms": 0,
                "articulation": "open",
                "role": "fill",
                "repeat_group": 0,
                "fill": True,
            },
            {
                "tick": 90,
                "lane": "floor_tom",
                "velocity": 116,
                "offset_ms": 2,
                "articulation": "open-accent",
                "role": "fill",
                "repeat_group": 0,
                "fill": True,
            },
        ]
        metric = metric_for_sample(sample)
        self.assertEqual(metric["performanceEventCount"], 4)
        self.assertTrue(metric["fillUsesThreeToms"])
        self.assertEqual(metric["fillTickCount"], 3)
        self.assertEqual(metric["maximumSimultaneousVoices"], 1)


if __name__ == "__main__":
    unittest.main()
