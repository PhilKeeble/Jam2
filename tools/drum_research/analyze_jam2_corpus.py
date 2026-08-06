#!/usr/bin/env python3
"""Measure repetition and development in Jam2's full-form music corpus."""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

if __package__:
    from .analyze_gmd import summarize
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.drum_research.analyze_gmd import summarize


CORE_LANES = {
    "Kick",
    "Snare",
    "Closed HH",
    "Open HH",
    "Ride",
    "Cross-stick / Rim",
}

LANE_IDS = {
    "Kick": "kick",
    "Snare": "snare",
    "Closed HH": "closed_hat",
    "Open HH": "open_hat",
    "Ride": "ride",
    "Crash": "crash",
    "High Tom": "high_tom",
    "Mid Tom": "mid_tom",
    "Floor Tom": "floor_tom",
    "Cross-stick / Rim": "cross_stick",
}


def ratio(numerator: int, denominator: int) -> float:
    return round(numerator / denominator, 4) if denominator else 0.0


def signature(
    hits: Iterable[dict[str, object]],
    start_tick: int,
    end_tick: int,
    lanes: set[str] | None = None,
) -> tuple[tuple[int, str, str], ...]:
    result = []
    for hit in hits:
        tick = int(hit["tick"])
        lane = str(hit["lane"])
        if start_tick <= tick < end_tick and (lanes is None or lane in lanes):
            result.append((tick - start_tick, lane, str(hit["state"])))
    return tuple(sorted(result))


def symmetric_change(
    left: tuple[tuple[int, str, str], ...],
    right: tuple[tuple[int, str, str], ...],
) -> int:
    return len(set(left).symmetric_difference(right))


def presence_signature(
    hits: Iterable[dict[str, object]],
    start_tick: int,
    end_tick: int,
    lanes: set[str] | None = None,
) -> tuple[tuple[int, str], ...]:
    return tuple(
        sorted(
            (
                int(hit["tick"]) - start_tick,
                str(hit["lane"]),
            )
            for hit in hits
            if start_tick <= int(hit["tick"]) < end_tick
            and (lanes is None or str(hit["lane"]) in lanes)
        )
    )


def signature_similarity(
    left: tuple[tuple[int, str], ...],
    right: tuple[tuple[int, str], ...],
) -> float:
    left_set = set(left)
    right_set = set(right)
    union = left_set | right_set
    return len(left_set & right_set) / len(union) if union else 1.0


def metric_for_sample(sample: dict[str, object]) -> dict[str, object]:
    recipe = sample["recipe"]
    bars = int(recipe["bars"])
    beats_per_bar = int(recipe["beats_per_bar"])
    ticks_per_bar = beats_per_bar * 12
    hits = sample["drum_hits"]
    all_bars = [
        signature(hits, bar * ticks_per_bar, (bar + 1) * ticks_per_bar)
        for bar in range(bars)
    ]
    core_bars = [
        signature(
            hits,
            bar * ticks_per_bar,
            (bar + 1) * ticks_per_bar,
            CORE_LANES,
        )
        for bar in range(bars)
    ]
    core_presence_bars = [
        presence_signature(
            hits,
            bar * ticks_per_bar,
            (bar + 1) * ticks_per_bar,
            CORE_LANES,
        )
        for bar in range(bars)
    ]
    period2_all = sum(
        all_bars[index] == all_bars[index - 2] for index in range(2, bars)
    )
    period2_core = sum(
        core_bars[index] == core_bars[index - 2] for index in range(2, bars)
    )
    period4_core = sum(
        core_bars[index] == core_bars[index - 4] for index in range(4, bars)
    )
    consecutive_core = sum(
        core_bars[index] == core_bars[index - 1] for index in range(1, bars)
    )
    changes = [
        symmetric_change(core_bars[index - 1], core_bars[index])
        for index in range(1, bars)
    ]
    adjacent_similarity = [
        signature_similarity(
            core_presence_bars[index - 1],
            core_presence_bars[index],
        )
        for index in range(1, bars)
    ]
    period2_similarity = [
        signature_similarity(
            core_presence_bars[index - 2],
            core_presence_bars[index],
        )
        for index in range(2, bars)
    ]
    period4_similarity = [
        signature_similarity(
            core_presence_bars[index - 4],
            core_presence_bars[index],
        )
        for index in range(4, bars)
    ]
    boundary_changes: dict[str, list[int]] = {}
    boundary_similarity: dict[str, list[float]] = {}
    for width in (2, 4, 8):
        boundary_changes[str(width)] = [
            changes[bar - 1] for bar in range(width, bars, width)
        ]
        boundary_similarity[str(width)] = [
            adjacent_similarity[bar - 1]
            for bar in range(width, bars, width)
        ]
    state_counts = Counter(str(hit["state"]).lower() for hit in hits)
    lane_counts = Counter(str(hit["lane"]) for hit in hits)
    groove = recipe["groove"]
    variation_counts = groove["variation_counts"]
    quarter = max(1, bars // 4)
    quarter_hits = []
    for first_bar in range(0, bars, quarter):
        end_bar = min(bars, first_bar + quarter)
        first_tick = first_bar * ticks_per_bar
        end_tick = end_bar * ticks_per_bar
        quarter_hits.append(
            sum(first_tick <= int(hit["tick"]) < end_tick for hit in hits)
        )
    performance = groove.get("performance_events", [])
    grid_states = {
        (int(hit["tick"]), LANE_IDS.get(str(hit["lane"]), "")): str(
            hit["state"]
        ).lower()
        for hit in hits
    }
    velocities = [int(event["velocity"]) for event in performance]
    offsets = [int(event["offset_ms"]) for event in performance]
    velocities_by_lane: dict[str, list[int]] = defaultdict(list)
    offsets_by_lane: dict[str, list[int]] = defaultdict(list)
    velocities_by_state: dict[str, list[int]] = defaultdict(list)
    articulation_counts: Counter[str] = Counter()
    articulations_by_lane: dict[str, Counter[str]] = defaultdict(Counter)
    performance_role_counts: Counter[str] = Counter()
    fill_lane_counts: Counter[str] = Counter()
    repeat_deltas: list[int] = []
    repeat_deltas_by_lane: dict[str, list[int]] = defaultdict(list)
    same_state_repeat_deltas: list[int] = []
    same_state_repeat_deltas_by_lane: dict[str, list[int]] = defaultdict(list)
    previous_by_lane: dict[str, tuple[int, int, str]] = {}
    simultaneous: Counter[int] = Counter()
    fill_ticks: set[int] = set()
    fill_events_by_beat: dict[int, list[tuple[int, str]]] = defaultdict(list)
    for event in performance:
        tick = int(event["tick"])
        lane = str(event["lane"])
        velocity = int(event["velocity"])
        offset = int(event["offset_ms"])
        velocities_by_lane[lane].append(velocity)
        offsets_by_lane[lane].append(offset)
        grid_state = grid_states.get((tick, lane), "unknown")
        velocities_by_state[grid_state].append(velocity)
        articulation_counts[str(event["articulation"])] += 1
        articulations_by_lane[lane][str(event["articulation"])] += 1
        performance_role_counts[str(event["role"])] += 1
        simultaneous[tick] += 1
        if bool(event["fill"]):
            fill_ticks.add(tick)
            fill_lane_counts[lane] += 1
            fill_events_by_beat[tick // 12].append(
                (tick, lane)
            )
        previous = previous_by_lane.get(lane)
        # Match the generator's local repeated-hit context: consecutive
        # strikes up to one written beat apart belong to the same physical
        # motion/rebound window.  The old three-tick cutoff only saw very
        # fast rolls and omitted ordinary eighth-note hats and Jazz Ride.
        if previous and tick - previous[0] <= 12:
            delta = abs(velocity - previous[1])
            repeat_deltas.append(delta)
            repeat_deltas_by_lane[lane].append(delta)
            if grid_state == previous[2]:
                same_state_repeat_deltas.append(delta)
                same_state_repeat_deltas_by_lane[lane].append(delta)
        previous_by_lane[lane] = (tick, velocity, grid_state)
    fill_tom_lanes = {
        lane
        for lane in fill_lane_counts
        if lane in {"high_tom", "mid_tom", "floor_tom"}
    }
    tom_rank = {"high_tom": 0, "mid_tom": 1, "floor_tom": 2}
    fill_tom_directions: Counter[str] = Counter()
    fill_spans: list[int] = []
    for fill_events in fill_events_by_beat.values():
        ordered = sorted(fill_events)
        fill_spans.append(
            ordered[-1][0] - ordered[0][0]
            if ordered
            else 0
        )
        toms = [
            tom_rank[lane]
            for _, lane in ordered
            if lane in tom_rank
        ]
        for previous, current in zip(toms, toms[1:]):
            fill_tom_directions[
                "descending"
                if current > previous
                else "ascending"
                if current < previous
                else "same"
            ] += 1
    bar_velocity = []
    for bar in range(bars):
        start = bar * ticks_per_bar
        end = (bar + 1) * ticks_per_bar
        values = [
            int(event["velocity"])
            for event in performance
            if start <= int(event["tick"]) < end
        ]
        bar_velocity.append(sum(values) / len(values) if values else 0.0)
    return {
        "id": sample["id"],
        "profile": sample["profile_id"],
        "form": sample["form_id"],
        "bars": bars,
        "complexity": int(sample["requested_complexity"]),
        "sampleIndex": int(sample["sample_index"]),
        "hits": len(hits),
        "hitsPerBar": len(hits) / bars,
        "uniqueBarRatio": ratio(len(set(all_bars)), bars),
        "uniqueCoreBarRatio": ratio(len(set(core_bars)), bars),
        "period2IdenticalRatio": ratio(period2_all, max(0, bars - 2)),
        "period2CoreIdenticalRatio": ratio(period2_core, max(0, bars - 2)),
        "period4CoreIdenticalRatio": ratio(period4_core, max(0, bars - 4)),
        "consecutiveCoreIdenticalRatio": ratio(
            consecutive_core, max(0, bars - 1)
        ),
        "adjacentCoreSymmetricChanges": changes,
        "adjacentCoreSimilarity": adjacent_similarity,
        "period2CoreSimilarity": period2_similarity,
        "period4CoreSimilarity": period4_similarity,
        "phraseBoundaryCoreChanges": boundary_changes,
        "phraseBoundaryCoreSimilarity": boundary_similarity,
        "quarterHitCounts": quarter_hits,
        "performanceEventCount": len(performance),
        "performanceEventsPerBar": len(performance) / bars,
        "performanceVelocities": velocities,
        "performanceOffsetsMs": offsets,
        "velocityByLane": dict(velocities_by_lane),
        "offsetByLane": dict(offsets_by_lane),
        "velocityByState": dict(velocities_by_state),
        "articulationCounts": dict(articulation_counts),
        "articulationCountsByLane": {
            lane: dict(counts)
            for lane, counts in sorted(articulations_by_lane.items())
        },
        "performanceRoleCounts": dict(performance_role_counts),
        "fillEventCount": sum(fill_lane_counts.values()),
        "fillTickCount": len(fill_ticks),
        "fillBeatCount": len(fill_events_by_beat),
        "fillSpanTicks": fill_spans,
        "fillTomDirectionCounts": dict(fill_tom_directions),
        "fillLaneCounts": dict(fill_lane_counts),
        "fillUsesThreeToms": fill_tom_lanes
        == {"high_tom", "mid_tom", "floor_tom"},
        "repeatVelocityAbsoluteDeltas": repeat_deltas,
        "repeatVelocityAbsoluteDeltasByLane": dict(
            repeat_deltas_by_lane
        ),
        "sameStateRepeatVelocityAbsoluteDeltas":
            same_state_repeat_deltas,
        "sameStateRepeatVelocityAbsoluteDeltasByLane": dict(
            same_state_repeat_deltas_by_lane
        ),
        "maximumSimultaneousVoices": max(simultaneous.values(), default=0),
        "barMeanVelocity": bar_velocity,
        "stateCounts": dict(state_counts),
        "laneCounts": dict(lane_counts),
        "variationCounts": variation_counts,
        "grooveId": groove["id"],
    }


@dataclass
class Aggregate:
    samples: int = 0
    hits_per_bar: list[float] = field(default_factory=list)
    unique_bar_ratio: list[float] = field(default_factory=list)
    unique_core_bar_ratio: list[float] = field(default_factory=list)
    period2_ratio: list[float] = field(default_factory=list)
    period2_core_ratio: list[float] = field(default_factory=list)
    period4_core_ratio: list[float] = field(default_factory=list)
    consecutive_core_ratio: list[float] = field(default_factory=list)
    adjacent_changes: list[float] = field(default_factory=list)
    adjacent_similarity: list[float] = field(default_factory=list)
    period2_similarity: list[float] = field(default_factory=list)
    period4_similarity: list[float] = field(default_factory=list)
    boundary_changes: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    boundary_similarity: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    fill_counts: list[float] = field(default_factory=list)
    kick_variations: list[float] = field(default_factory=list)
    ghost_variations: list[float] = field(default_factory=list)
    cymbal_variations: list[float] = field(default_factory=list)
    advanced_cells: list[float] = field(default_factory=list)
    states: Counter[str] = field(default_factory=Counter)
    lanes: Counter[str] = field(default_factory=Counter)
    groove_ids: Counter[str] = field(default_factory=Counter)
    performance_events_per_bar: list[float] = field(default_factory=list)
    performance_velocities: list[float] = field(default_factory=list)
    performance_offsets: list[float] = field(default_factory=list)
    velocities_by_lane: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    offsets_by_lane: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    velocities_by_state: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    articulations: Counter[str] = field(default_factory=Counter)
    articulations_by_lane: dict[str, Counter[str]] = field(
        default_factory=lambda: defaultdict(Counter)
    )
    performance_roles: Counter[str] = field(default_factory=Counter)
    fill_events: list[float] = field(default_factory=list)
    fill_ticks: list[float] = field(default_factory=list)
    fill_beats: list[float] = field(default_factory=list)
    fill_spans: list[float] = field(default_factory=list)
    fill_tom_directions: Counter[str] = field(default_factory=Counter)
    fill_lanes: Counter[str] = field(default_factory=Counter)
    three_tom_fills: int = 0
    repeat_velocity_deltas: list[float] = field(default_factory=list)
    repeat_velocity_deltas_by_lane: dict[str, list[float]] = field(
        default_factory=lambda: defaultdict(list)
    )
    same_state_repeat_velocity_deltas: list[float] = field(
        default_factory=list
    )
    same_state_repeat_velocity_deltas_by_lane: dict[
        str, list[float]
    ] = field(default_factory=lambda: defaultdict(list))
    maximum_simultaneous_voices: list[float] = field(default_factory=list)
    bar_mean_velocity: list[float] = field(default_factory=list)

    def add(self, metric: dict[str, object]) -> None:
        self.samples += 1
        self.hits_per_bar.append(float(metric["hitsPerBar"]))
        self.unique_bar_ratio.append(float(metric["uniqueBarRatio"]))
        self.unique_core_bar_ratio.append(float(metric["uniqueCoreBarRatio"]))
        self.period2_ratio.append(float(metric["period2IdenticalRatio"]))
        self.period2_core_ratio.append(float(metric["period2CoreIdenticalRatio"]))
        self.period4_core_ratio.append(float(metric["period4CoreIdenticalRatio"]))
        self.consecutive_core_ratio.append(
            float(metric["consecutiveCoreIdenticalRatio"])
        )
        self.adjacent_changes.extend(metric["adjacentCoreSymmetricChanges"])
        self.adjacent_similarity.extend(metric["adjacentCoreSimilarity"])
        self.period2_similarity.extend(metric["period2CoreSimilarity"])
        self.period4_similarity.extend(metric["period4CoreSimilarity"])
        for width, values in metric["phraseBoundaryCoreChanges"].items():
            self.boundary_changes[width].extend(values)
        for width, values in metric[
            "phraseBoundaryCoreSimilarity"
        ].items():
            self.boundary_similarity[width].extend(values)
        variations = metric["variationCounts"]
        self.fill_counts.append(float(variations["fill"]))
        self.kick_variations.append(float(variations["kick"]))
        self.ghost_variations.append(float(variations["ghost"]))
        self.cymbal_variations.append(float(variations["cymbal"]))
        self.advanced_cells.append(float(variations["advanced_cells"]))
        self.states.update(metric["stateCounts"])
        self.lanes.update(metric["laneCounts"])
        self.groove_ids[str(metric["grooveId"])] += 1
        self.performance_events_per_bar.append(
            float(metric["performanceEventsPerBar"])
        )
        self.performance_velocities.extend(metric["performanceVelocities"])
        self.performance_offsets.extend(metric["performanceOffsetsMs"])
        for lane, values in metric["velocityByLane"].items():
            self.velocities_by_lane[lane].extend(values)
        for lane, values in metric["offsetByLane"].items():
            self.offsets_by_lane[lane].extend(values)
        for state, values in metric["velocityByState"].items():
            self.velocities_by_state[state].extend(values)
        self.articulations.update(metric["articulationCounts"])
        for lane, counts in metric["articulationCountsByLane"].items():
            self.articulations_by_lane[lane].update(counts)
        self.performance_roles.update(metric["performanceRoleCounts"])
        self.fill_events.append(float(metric["fillEventCount"]))
        self.fill_ticks.append(float(metric["fillTickCount"]))
        self.fill_beats.append(float(metric["fillBeatCount"]))
        self.fill_spans.extend(metric["fillSpanTicks"])
        self.fill_tom_directions.update(
            metric["fillTomDirectionCounts"]
        )
        self.fill_lanes.update(metric["fillLaneCounts"])
        self.three_tom_fills += bool(metric["fillUsesThreeToms"])
        self.repeat_velocity_deltas.extend(
            metric["repeatVelocityAbsoluteDeltas"]
        )
        for lane, values in metric[
            "repeatVelocityAbsoluteDeltasByLane"
        ].items():
            self.repeat_velocity_deltas_by_lane[lane].extend(values)
        self.same_state_repeat_velocity_deltas.extend(
            metric["sameStateRepeatVelocityAbsoluteDeltas"]
        )
        for lane, values in metric[
            "sameStateRepeatVelocityAbsoluteDeltasByLane"
        ].items():
            self.same_state_repeat_velocity_deltas_by_lane[lane].extend(
                values
            )
        self.maximum_simultaneous_voices.append(
            float(metric["maximumSimultaneousVoices"])
        )
        self.bar_mean_velocity.extend(metric["barMeanVelocity"])

    def json(self) -> dict[str, object]:
        return {
            "samples": self.samples,
            "hitsPerBar": summarize(self.hits_per_bar),
            "uniqueBarRatio": summarize(self.unique_bar_ratio),
            "uniqueCoreBarRatio": summarize(self.unique_core_bar_ratio),
            "period2IdenticalRatio": summarize(self.period2_ratio),
            "period2CoreIdenticalRatio": summarize(self.period2_core_ratio),
            "period4CoreIdenticalRatio": summarize(self.period4_core_ratio),
            "consecutiveCoreIdenticalRatio": summarize(
                self.consecutive_core_ratio
            ),
            "adjacentCoreSymmetricChanges": summarize(self.adjacent_changes),
            "adjacentCoreSimilarity": summarize(self.adjacent_similarity),
            "period2CoreSimilarity": summarize(self.period2_similarity),
            "period4CoreSimilarity": summarize(self.period4_similarity),
            "phraseBoundaryCoreChanges": {
                width: summarize(values)
                for width, values in sorted(self.boundary_changes.items())
            },
            "phraseBoundaryCoreSimilarity": {
                width: summarize(values)
                for width, values in sorted(
                    self.boundary_similarity.items()
                )
            },
            "variationCountsPerIdea": {
                "fill": summarize(self.fill_counts),
                "kick": summarize(self.kick_variations),
                "ghost": summarize(self.ghost_variations),
                "cymbal": summarize(self.cymbal_variations),
                "advancedCells": summarize(self.advanced_cells),
            },
            "stateCounts": dict(self.states),
            "laneCounts": dict(self.lanes),
            "grooveIds": dict(self.groove_ids),
            "performance": {
                "eventsPerBar": summarize(self.performance_events_per_bar),
                "velocity": summarize(self.performance_velocities),
                "offsetMs": summarize(self.performance_offsets),
                "velocityByLane": {
                    lane: summarize(values)
                    for lane, values in sorted(
                        self.velocities_by_lane.items()
                    )
                },
                "offsetMsByLane": {
                    lane: summarize(values)
                    for lane, values in sorted(self.offsets_by_lane.items())
                },
                "velocityByGridState": {
                    state: summarize(values)
                    for state, values in sorted(
                        self.velocities_by_state.items()
                    )
                },
                "articulationCounts": dict(self.articulations),
                "articulationCountsByLane": {
                    lane: dict(counts)
                    for lane, counts in sorted(
                        self.articulations_by_lane.items()
                    )
                },
                "roleCounts": dict(self.performance_roles),
                "fillEventsPerIdea": summarize(self.fill_events),
                "fillTicksPerIdea": summarize(self.fill_ticks),
                "fillBeatsPerIdea": summarize(self.fill_beats),
                "fillSpanTicks": summarize(self.fill_spans),
                "fillTomDirectionCounts": dict(
                    self.fill_tom_directions
                ),
                "fillLaneCounts": dict(self.fill_lanes),
                "ideasWithThreeTomFills": self.three_tom_fills,
                "repeatVelocityAbsoluteDelta": summarize(
                    self.repeat_velocity_deltas
                ),
                "repeatVelocityAbsoluteDeltaByLane": {
                    lane: summarize(values)
                    for lane, values in sorted(
                        self.repeat_velocity_deltas_by_lane.items()
                    )
                },
                "sameStateRepeatVelocityAbsoluteDelta": summarize(
                    self.same_state_repeat_velocity_deltas
                ),
                "sameStateRepeatVelocityAbsoluteDeltaByLane": {
                    lane: summarize(values)
                    for lane, values in sorted(
                        self.same_state_repeat_velocity_deltas_by_lane.items()
                    )
                },
                "maximumSimultaneousVoices": summarize(
                    self.maximum_simultaneous_voices
                ),
                "barMeanVelocity": summarize(self.bar_mean_velocity),
            },
        }


def analyze(path: Path) -> dict[str, object]:
    corpus = json.loads(path.read_text(encoding="utf-8"))
    metrics = [metric_for_sample(sample) for sample in corpus["samples"]]
    overall = Aggregate()
    profiles: dict[str, Aggregate] = defaultdict(Aggregate)
    complexities: dict[int, Aggregate] = defaultdict(Aggregate)
    lengths: dict[int, Aggregate] = defaultdict(Aggregate)
    profile_complexities: dict[str, Aggregate] = defaultdict(Aggregate)
    for metric in metrics:
        aggregates = (
            overall,
            profiles[str(metric["profile"])],
            complexities[int(metric["complexity"])],
            lengths[int(metric["bars"])],
            profile_complexities[
                f"{metric['profile']}|c{metric['complexity']}"
            ],
        )
        for aggregate in aggregates:
            aggregate.add(metric)
    return {
        "schema": "jam2-generated-drum-corpus-audit-v2",
        "source": str(path),
        "corpusVersion": corpus.get("version"),
        "method": {
            "coreLanes": sorted(CORE_LANES),
            "note": (
                "Reports raw repetition, density, state and authored mutation "
                "measurements. No subjective groove or playability score is used."
            ),
        },
        "overall": overall.json(),
        "byProfile": {
            key: value.json() for key, value in sorted(profiles.items())
        },
        "byComplexity": {
            str(key): value.json() for key, value in sorted(complexities.items())
        },
        "byLength": {
            str(key): value.json() for key, value in sorted(lengths.items())
        },
        "byProfileComplexity": {
            key: value.json()
            for key, value in sorted(profile_complexities.items())
        },
        "samples": metrics,
    }


def print_summary(result: dict[str, object]) -> None:
    print(
        "| Complexity | Samples | Hits/bar | Unique core bars | "
        "2-bar core repeats | Fills/idea |"
    )
    print("|---:|---:|---:|---:|---:|---:|")
    for complexity, item in result["byComplexity"].items():
        print(
            f"| {complexity} | {item['samples']} | "
            f"{item['hitsPerBar']['median']} | "
            f"{item['uniqueCoreBarRatio']['median']} | "
            f"{item['period2CoreIdenticalRatio']['median']} | "
            f"{item['variationCountsPerIdea']['fill']['median']} |"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.corpus)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print_summary(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
