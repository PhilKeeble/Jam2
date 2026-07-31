#!/usr/bin/env python3
"""Extract the feedback-selected Drum Kit Lab defaults for Jam2 production.

The experiment manifest remains the complete A/B/C workbench. Production
embeds only the recommended complete kit for each profile. No rendered audio,
external MIDI, or third-party sample is copied by this step.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


PREFIX = "window.JAM2_SOUND_DESIGN_MANIFEST = "


def read_manifest(path: Path) -> dict:
    text = path.read_text(encoding="utf-8").strip()
    if not text.startswith(PREFIX):
        raise ValueError(f"{path} is not a sound-design manifest")
    payload = text[len(PREFIX) :]
    if payload.endswith(";"):
        payload = payload[:-1]
    return json.loads(payload)


def extract(manifest: dict) -> dict:
    profiles: dict[str, dict] = {}
    for profile in manifest.get("profiles", []):
        drums = next(
            (
                role
                for role in profile.get("roles", [])
                if role.get("id") == "drums"
            ),
            None,
        )
        if not drums:
            raise ValueError(f"profile {profile.get('id')} has no drums role")
        selected = next(
            (
                kit
                for kit in drums.get("kitCandidates", [])
                if kit.get("recommended") is True
            ),
            None,
        )
        if not selected:
            raise ValueError(
                f"profile {profile.get('id')} has no recommended drum kit"
            )
        parameters = selected.get("parameters", {})
        profiles[profile["id"]] = {
            "kit_id": selected["id"],
            "kit_name": selected["name"],
            "research_family": selected.get("researchFamily", ""),
            "bus": parameters.get("bus", {}),
            "pieces": parameters.get("pieces", {}),
        }
    return {
        "schema": 1,
        "revision": 1,
        "source_generated_at": manifest.get("generatedAt", ""),
        "profiles": profiles,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = extract(read_manifest(args.manifest))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"wrote {len(data['profiles'])} recommended kits to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
