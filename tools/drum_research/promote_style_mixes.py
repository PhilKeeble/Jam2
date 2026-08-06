#!/usr/bin/env python3
"""Promote the two accepted Synth A/B drum kits into Jam2's runtime catalog."""

from __future__ import annotations

import copy
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HANDOFF = ROOT / "experiments" / "synth-ab" / "presets" / "style-mixes.json"
OUTPUT = ROOT / "app" / "gui" / "researched-drum-kits.json"

PROFILE_KITS = {
    "blues_dominant": "acoustic",
    "blues_minor": "acoustic",
    "bossa_songbook": "acoustic",
    "country_contemporary": "acoustic",
    "country_honky_tonk": "acoustic",
    "electronic_breakbeat": "electronic",
    "electronic_house": "electronic",
    "electronic_techno": "electronic",
    "funk_static_pocket": "acoustic",
    "hiphop_boom_bap": "electronic",
    "hiphop_trap": "electronic",
    "jazz_bebop": "acoustic",
    "jazz_fusion": "acoustic",
    "jazz_swing_standards": "acoustic",
    "jpop_anisong_rock": "electronic",
    "jpop_idol_dance": "electronic",
    "metal_modern_progressive": "electronic",
    "modal_atmospheric": "acoustic",
    "modal_groove": "acoustic",
    "pop_loop": "acoustic",
    "pop_sectional": "acoustic",
    "reggae_roots": "acoustic",
    "rnb_contemporary_neosoul": "electronic",
    "rock_punk_garage": "acoustic",
    "rock_riff_modal": "acoustic",
    "rock_shuffle_blues": "acoustic",
    "soul_classic_motown": "electronic",
}

DRONE_PROFILES = {
    "electronic_techno",
    "modal_atmospheric",
    "modal_groove",
}

RIDE_HEAVY_STYLES = {"jazz", "modal-jam"}


def drum_role(style_mix: dict) -> dict:
    roles = [role for role in style_mix["roles"] if role["role"] == "drums"]
    if len(roles) != 1:
        raise ValueError(f"{style_mix['profileId']}: expected exactly one drum role")
    return roles[0]


def product_name(kit_id: str) -> str:
    return "Acoustic" if kit_id == "acoustic" else "Electronic"


def sanitize_text(value, kit_id: str):
    if not isinstance(value, str):
        return value
    return (value.replace("AbletonRock32", "Acoustic")
                 .replace("Ableton 32 Pad Kit Rock", "Acoustic kit")
                 .replace("Ableton808", "Electronic")
                 .replace("Ableton 808 Core Kit", "Electronic kit"))


def sanitize_piece(piece: dict, kit_id: str) -> dict:
    result = copy.deepcopy(piece)
    result["intendedIdentity"] = sanitize_text(
        result.get("intendedIdentity", ""), kit_id)
    return result


def damp_ride(pieces: dict, style_id: str) -> None:
    if style_id not in RIDE_HEAVY_STYLES or "ride" not in pieces:
        return
    ride = pieces["ride"]
    ride["level"] = min(float(ride.get("level", 0.34)),
                        0.27 if style_id == "jazz" else 0.25)
    ride["decay"] = min(float(ride.get("decay", 0.9)), 0.72)
    detail_already_damped = (
        float(ride.get("onsetSofteningSeconds", 0.0)) >= 0.022 and
        float(ride.get("colourStage", {}).get(
            "reconstructionLowpassHz", 20000.0)) <= 6500.0
    )
    ride["onsetSofteningSeconds"] = max(
        float(ride.get("onsetSofteningSeconds", 0.0)), 0.022)
    colour = ride.setdefault("colourStage", {})
    colour["reconstructionLowpassHz"] = min(
        float(colour.get("reconstructionLowpassHz", 20000.0)), 6500.0)
    if not detail_already_damped:
        transient = ride.setdefault("transient", {})
        transient["level"] = float(transient.get("level", 0.0)) * 0.55
        transient["tone"] = float(transient.get("tone", 0.5)) * 0.75
        texture = ride.setdefault("texture", {})
        texture["level"] = float(texture.get("level", 0.0)) * 0.75
        texture["tone"] = float(texture.get("tone", 0.5)) * 0.80
        texture["decaySeconds"] = min(
            float(texture.get("decaySeconds", 0.0)), 1.45)
        for band in ride.get("modalBands", []):
            band["decaySeconds"] = float(band.get("decaySeconds", 0.0)) * 0.82
            if float(band.get("frequencyHz", 0.0)) >= 4000.0:
                band["level"] = float(band.get("level", 0.0)) * 0.58
            normal = float(band.get("normalGain", 1.0))
            band["accentGain"] = min(float(band.get("accentGain", 1.0)), normal * 1.05)
        for band in ride.get("noiseBands", []):
            if float(band.get("frequencyHz", 0.0)) >= 4000.0:
                band["level"] = float(band.get("level", 0.0)) * 0.68
            band["accentGain"] = min(float(band.get("accentGain", 1.0)), 1.10)


def sanitize_handoff(document: dict) -> dict:
    mixes = document.get("styleMixes", {})
    if set(mixes) != set(PROFILE_KITS):
        missing = sorted(set(PROFILE_KITS) - set(mixes))
        extra = sorted(set(mixes) - set(PROFILE_KITS))
        raise ValueError(f"style mix coverage mismatch: missing={missing}, extra={extra}")
    for profile_id, mix in mixes.items():
        if profile_id in DRONE_PROFILES:
            mix["roles"] = [role for role in mix["roles"] if role["role"] != "support"]
            mix.get("mix", {}).get("roles", {}).pop("support", None)
        drums = drum_role(mix)
        expected = PROFILE_KITS[profile_id]
        selection = drums["selection"]
        selection["baseKitId"] = expected
        selection["baseKitName"] = product_name(expected)
        selection["treatmentId"] = mix["styleId"]
        for piece_id, piece_selection in selection.get("pieces", {}).items():
            piece_selection.update({
                "choiceId": "",
                "kitId": expected,
                "name": f"{product_name(expected)} {piece_id.replace('-', ' ').title()}",
                "source": "selected-kit",
            })
        parameters = drums["parameters"]
        parameters["candidateId"] = f"base-{expected}-{mix['styleId']}"
        parameters["candidateName"] = f"{product_name(expected)} / {mix['styleId']}"
        parameters["description"] = (
            f"Jam2 {product_name(expected).lower()} procedural kit with "
            f"the {mix['styleId']} mix treatment."
        )
        parameters["researchFamily"] = f"jam2-{expected}"
        parameters["sourceReferences"] = [
            f"Jam2 {product_name(expected)} procedural drum design"
        ]
        parameters["pieces"] = {
            name: sanitize_piece(piece, expected)
            for name, piece in parameters["pieces"].items()
        }
        damp_ride(parameters["pieces"], mix["styleId"])
    return document


def neutral_base(mixes: dict, kit_id: str) -> dict:
    if kit_id == "acoustic":
        source = drum_role(mixes["modal_atmospheric"])["parameters"]
        result = copy.deepcopy(source)
        result["bus"] = {
            "drive": 1.08, "lowpassHz": 12600,
            "compressorThreshold": 0.18, "compressorRatio": 1.7,
            "compressorReleaseMs": 86, "roomMix": 0.055,
            "roomSizeMs": 34, "roomDamping": 0.62,
        }
        result["pieces"]["kick"]["tone"] = 0.14
        result["pieces"]["crash"]["decay"] = 1.0
    else:
        source = drum_role(mixes["jpop_anisong_rock"])["parameters"]
        result = copy.deepcopy(source)
        result["bus"] = {
            "drive": 1.0, "lowpassHz": 20000,
            "compressorThreshold": 0.2, "compressorRatio": 2.5,
            "compressorReleaseMs": 48, "roomMix": 0.0,
            "roomSizeMs": 21, "roomDamping": 0.8,
        }
        snare = result["pieces"]["snare"]
        snare.update({"tone": 0.08, "blend": 0.0})
        result["pieces"]["closed-hat"]["level"] = 0.58
    result["candidateId"] = kit_id
    result["candidateName"] = product_name(kit_id)
    result["description"] = f"Jam2 {kit_id} base drum kit."
    result["researchFamily"] = f"jam2-{kit_id}"
    result["sourceReferences"] = [f"Jam2 {product_name(kit_id)} procedural drum design"]
    return result


def runtime_catalog(document: dict) -> dict:
    profiles = {}
    for profile_id, mix in document["styleMixes"].items():
        drums = drum_role(mix)
        parameters = copy.deepcopy(drums["parameters"])
        profiles[profile_id] = {
            "kit_id": parameters.pop("candidateId"),
            "base_kit_id": PROFILE_KITS[profile_id],
            "treatment_id": mix["styleId"],
            "kit_name": parameters.pop("candidateName"),
            "research_family": parameters.pop("researchFamily"),
            "bus": parameters["bus"],
            "pieces": parameters["pieces"],
        }
    bases = {}
    for kit_id in ("acoustic", "electronic"):
        parameters = neutral_base(document["styleMixes"], kit_id)
        bases[kit_id] = {
            "kit_id": parameters["candidateId"],
            "base_kit_id": kit_id,
            "treatment_id": "neutral",
            "kit_name": parameters["candidateName"],
            "research_family": parameters["researchFamily"],
            "bus": parameters["bus"],
            "pieces": parameters["pieces"],
        }
    return {
        "schema": 2,
        "revision": 2,
        "base_kits": bases,
        "profiles": profiles,
    }


def main() -> None:
    document = json.loads(HANDOFF.read_text(encoding="utf-8"))
    if document.get("schema") != "jam2-style-mix-handoff-collection-v1":
        raise ValueError("unsupported style mix handoff schema")
    document = sanitize_handoff(document)
    HANDOFF.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    OUTPUT.write_text(json.dumps(runtime_catalog(document), indent=2) + "\n", encoding="utf-8")
    print(f"promoted {len(PROFILE_KITS)} profile treatments and 2 base kits")


if __name__ == "__main__":
    main()
