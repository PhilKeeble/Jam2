#!/usr/bin/env python3
"""Write a compact human-review workbook for one style in a Jam2 corpus."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

if __package__:
    from .analyze_jam2_generation import (
        role_events,
        section_density_changes,
        selected_complexity_tools,
    )
else:
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.music_research.analyze_jam2_generation import (
        role_events,
        section_density_changes,
        selected_complexity_tools,
    )


def compact_counter(values: Iterable[str]) -> str:
    counts = Counter(value for value in values if value)
    return ", ".join(
        f"{name}×{count}" if count > 1 else name
        for name, count in sorted(counts.items())
    ) or "none"


def roman_plan(recipe: dict[str, Any]) -> str:
    harmony = recipe.get("progression", {}).get("base_harmony", [])
    return " · ".join(
        f"{event.get('beat', 0)}:{event.get('roman', '?')}"
        f"({event.get('duration_beats', 0)})"
        for event in harmony
    )


def melodic_preview(events: list[dict[str, Any]], melody: bool) -> str:
    if not events:
        return "none"
    first = int(events[0].get("midi", 0))
    result = []
    for event in events[:16]:
        role = (
            event.get("melodic_role", "")
            if melody
            else event.get("relationship", "") or event.get("role", "")
        )
        result.append(
            f"{event.get('tick', 0)}:"
            f"{int(event.get('midi', 0)) - first:+d}/"
            f"{event.get('duration_ticks', 0)}"
            f"[{role}]"
        )
    if len(events) > 16:
        result.append(f"…+{len(events) - 16}")
    return " ".join(result)


def drum_summary(recipe: dict[str, Any]) -> str:
    events = recipe.get("groove", {}).get("performance_events", [])
    lanes = compact_counter(str(event.get("lane", "")) for event in events)
    fills = sum(bool(event.get("fill", False)) for event in events)
    ghosts = sum(int(event.get("velocity", 0)) <= 38 for event in events)
    return f"{len(events)} events; {fills} fill hits; {ghosts} low-velocity hits; {lanes}"


def section_summary(recipe: dict[str, Any]) -> str:
    _, rows = section_density_changes(recipe)
    return " | ".join(
        f"{row['label']}={row['role']} "
        + ",".join(
            f"{lane}:{value:g}"
            for lane, value in row["events_per_bar"].items()
        )
        for row in rows
    )


def matched_group_markdown(samples: list[dict[str, Any]]) -> list[str]:
    ordered = sorted(
        samples, key=lambda sample: int(sample["requested_complexity"])
    )
    lines = [
        "| Complexity | Tonic/mode | BPM | Progression | Groove | Variation | Selected concepts | M/B/S events |",
        "|---:|---|---:|---|---|---|---|---:|",
    ]
    for sample in ordered:
        recipe = sample["recipe"]
        lines.append(
            "| {complexity} | {tonic} {mode} | {bpm} | {progression} | "
            "{groove} | {variation} | {tools} | {melody}/{bass}/{support} |".format(
                complexity=sample["requested_complexity"],
                tonic=recipe.get("tonic", ""),
                mode=recipe.get("mode", ""),
                bpm=recipe.get("bpm", 0),
                progression=recipe.get("progression", {}).get("id", ""),
                groove=recipe.get("groove", {}).get("id", ""),
                variation=recipe.get("variation", {}).get("summary", ""),
                tools=", ".join(sorted(selected_complexity_tools(recipe))),
                melody=len(role_events(recipe, "melody")),
                bass=len(role_events(recipe, "bass")),
                support=len(role_events(recipe, "support")),
            )
        )
    lines.append("")
    for sample in ordered:
        recipe = sample["recipe"]
        complexity = sample["requested_complexity"]
        melody = role_events(recipe, "melody")
        bass = role_events(recipe, "bass")
        support = role_events(recipe, "support")
        lines.extend(
            [
                f"Complexity {complexity}:",
                "",
                f"- Harmony: {roman_plan(recipe)}",
                (
                    "- Motif: "
                    f"`{recipe.get('motif', {}).get('cell', '')}`; rhythm "
                    f"`{recipe.get('motif', {}).get('rhythm', '')}`; form "
                    f"`{recipe.get('motif', {}).get('form', '')}`."
                ),
                f"- Melody preview (tick:interval/duration[role]): {melodic_preview(melody, True)}",
                f"- Bass preview: {melodic_preview(bass, False)}",
                (
                    "- Bass relationships: "
                    + compact_counter(
                        str(event.get("relationship", "")) for event in bass
                    )
                    + "."
                ),
                f"- Support preview: {melodic_preview(support, False)}",
                (
                    "- Support roles: "
                    + compact_counter(
                        str(event.get("role", "")) for event in support
                    )
                    + "."
                ),
                f"- Drums: {drum_summary(recipe)}.",
                f"- Section densities: {section_summary(recipe)}.",
                "",
            ]
        )
    return lines


def make_review(
    corpus: dict[str, Any],
    style_id: str,
    samples_per_cell: int,
) -> str:
    selected = [
        sample
        for sample in corpus.get("samples", [])
        if sample.get("style_id") == style_id
        and int(sample.get("sample_index", 0)) < samples_per_cell
    ]
    if not selected:
        raise ValueError(f"style {style_id!r} is absent from the corpus")
    by_profile_form_seed: dict[
        tuple[str, str, int], list[dict[str, Any]]
    ] = defaultdict(list)
    for sample in selected:
        by_profile_form_seed[
            (
                str(sample["profile_id"]),
                str(sample["form_id"]),
                int(sample["sample_index"]),
            )
        ].append(sample)
    lines = [
        f"# Jam2 Style Review — {style_id}",
        "",
        f"- Corpus seed namespace: `{corpus.get('seed_namespace', '')}`",
        f"- Matched complexity seeds: `{corpus.get('matched_complexity_seeds', False)}`",
        f"- Reviewed seed groups: `{len(by_profile_form_seed)}`",
        "- Timbre and mix are excluded.",
        "",
    ]
    current_profile = None
    current_form = None
    for (profile_id, form_id, sample_index), samples in sorted(
        by_profile_form_seed.items()
    ):
        profile_name = str(samples[0].get("profile_name", profile_id))
        if profile_id != current_profile:
            current_profile = profile_id
            current_form = None
            lines.extend([f"## {profile_name} (`{profile_id}`)", ""])
        if form_id != current_form:
            current_form = form_id
            lines.extend(
                [
                    f"### {samples[0].get('form_name', form_id)} (`{form_id}`)",
                    "",
                ]
            )
        lines.extend(
            [
                f"#### Seed group {sample_index + 1} (`{samples[0].get('seed_group_id', '')}`)",
                "",
                *matched_group_markdown(samples),
            ]
        )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("style_id")
    parser.add_argument("--samples-per-cell", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with args.corpus.open("r", encoding="utf-8") as source:
        corpus = json.load(source)
    review = make_review(
        corpus,
        args.style_id,
        max(1, min(args.samples_per_cell, 16)),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(review, encoding="utf-8")
    print(f"Wrote {args.style_id} review to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

