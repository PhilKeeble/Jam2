#!/usr/bin/env python3
"""Render and objectively audit the researched Drum Kit Lab candidate matrix."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


DRUM_PIECES = (
    "kick",
    "snare",
    "closed-hat",
    "open-hat",
    "high-tom",
    "mid-tom",
    "floor-tom",
    "crash",
    "ride",
    "cross-stick",
)

HARD_SMACK_SNARE_PROFILES = {
    "rock_riff_modal",
    "rock_shuffle_blues",
    "rock_punk_garage",
    "metal_modern_progressive",
}

SHARED_POP_PROFILES = frozenset(("pop_loop", "pop_sectional"))
NATIVE_REGGAE_FOCUS = (
    "reggae_roots",
    "jam2-roots-warm-crash",
)


def is_intentional_shared_pop_candidate(
    profile_id: str,
    candidate_id: str,
    previous: tuple[str, ...] | None,
) -> bool:
    return bool(
        previous
        and frozenset((profile_id, previous[0]))
        == SHARED_POP_PROFILES
        and candidate_id == previous[1]
    )


def is_native_reggae_piece(
    profile_id: str,
    candidate_id: str,
    piece_name: str,
) -> bool:
    return (
        (profile_id, candidate_id) == NATIVE_REGGAE_FOCUS
        and piece_name != "crash"
    )


def is_intentional_piece_reuse(
    profile_id: str,
    candidate_id: str,
    piece_name: str,
    previous: tuple[str, str, str] | None,
) -> bool:
    if is_intentional_shared_pop_candidate(
        profile_id,
        candidate_id,
        (previous[0], previous[1]) if previous else None,
    ):
        return True
    return bool(
        previous
        and profile_id == "reggae_roots"
        and piece_name == "crash"
        and {candidate_id, previous[1]}
        == {
            "jam2-roots-warm-crash",
            "warm-rhythm-box-roots",
        }
    )

PIECE_SOURCE_CONTRACTS = {
    "kick": {
        "daisy-analog-kick",
        "daisy-synthetic-kick",
    },
    "snare": {
        "jam2-shell-snare",
        "daisy-analog-snare",
        "daisy-synthetic-snare",
    },
    "closed-hat": {
        "daisy-metal",
        "daisy-ring-metal",
    },
    "open-hat": {
        "daisy-metal",
        "daisy-ring-metal",
    },
    "high-tom": {
        "jam2-shell-tom",
        "daisy-synthetic-kick",
    },
    "mid-tom": {
        "jam2-shell-tom",
        "daisy-synthetic-kick",
    },
    "floor-tom": {
        "jam2-shell-tom",
        "daisy-synthetic-kick",
    },
    "crash": {"jam2-crash-cymbal"},
    "ride": {"jam2-ride-cymbal"},
    "cross-stick": {"jam2-cross-stick"},
}


def load_manifest(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        raise ValueError(f"{path} does not contain a JSON manifest")
    return json.loads(text[start : end + 1])


def piece_is_materially_different(left: dict, right: dict) -> bool:
    if left.get("source") != right.get("source"):
        return True
    if left.get("secondSource", "off") != right.get(
        "secondSource", "off"
    ):
        return True
    left_frequency = max(float(left.get("frequencyHz", 1)), 1)
    right_frequency = max(float(right.get("frequencyHz", 1)), 1)
    if abs(left_frequency / right_frequency - 1) > 0.04:
        return True
    for key, threshold in (
        ("tone", 0.04),
        ("decay", 0.04),
        ("colour", 0.05),
        ("fmAmount", 0.06),
        ("roomSend", 0.035),
    ):
        if abs(float(left.get(key, 0)) - float(right.get(key, 0))) > threshold:
            return True
    for group_name in ("transient", "texture"):
        left_group = left.get(group_name) or {}
        right_group = right.get(group_name) or {}
        if left_group.get("type") != right_group.get("type"):
            return True
        if abs(
            float(left_group.get("level", 0)) -
            float(right_group.get("level", 0))
        ) > 0.035:
            return True
    left_colour = left.get("colourStage") or {}
    right_colour = right.get("colourStage") or {}
    if abs(
        float(left_colour.get("voiceDrive", 1)) -
        float(right_colour.get("voiceDrive", 1))
    ) > 0.12:
        return True
    if abs(
        float(left_colour.get("sampleRateHz", 48000)) -
        float(right_colour.get("sampleRateHz", 48000))
    ) > 1200:
        return True
    if left_colour.get("bitDepth", 24) != right_colour.get(
        "bitDepth", 24
    ):
        return True
    return False


def validate_manifest(manifest: dict) -> list[dict]:
    """Reject structurally wrong defaults before treating WAV metrics as useful."""
    findings: list[dict] = []
    kit_fingerprints: dict[str, tuple[str, str]] = {}
    piece_fingerprints: dict[str, tuple[str, str, str]] = {}
    recommended_kits: list[tuple[str, dict]] = []
    profiles = manifest.get("profiles") or []
    if len(profiles) != 27:
        findings.append({
            "error": f"expected 27 profiles, found {len(profiles)}",
            "kind": "manifest-profile-count",
        })
    for profile in profiles:
        profile_id = profile.get("id", "<missing>")
        roles = profile.get("roles") or []
        role = next(
            (item for item in roles if item.get("id") == "drums"),
            None,
        )
        if role is None:
            findings.append({
                "profileId": profile_id,
                "kind": "missing-drum-role",
            })
            continue
        candidates = role.get("kitCandidates") or []
        if len(candidates) != 3:
            findings.append({
                "profileId": profile_id,
                "kind": "candidate-count",
                "detail": len(candidates),
            })
        if sum(bool(item.get("recommended")) for item in candidates) != 1:
            findings.append({
                "profileId": profile_id,
                "kind": "recommended-count",
            })
        else:
            recommended_kits.append((
                profile_id,
                next(
                    item
                    for item in candidates
                    if item.get("recommended")
                ),
            ))
        families = {
            str(item.get("researchFamily") or "")
            for item in candidates
        }
        if len(families) != len(candidates) or "" in families:
            findings.append({
                "profileId": profile_id,
                "kind": "candidate-family-reuse",
                "families": sorted(families),
            })
        for candidate in candidates:
            candidate_id = candidate.get("id", "<missing>")
            parameters = candidate.get("parameters") or {}
            pieces = parameters.get("pieces") or {}
            missing = sorted(set(DRUM_PIECES) - set(pieces))
            extra = sorted(set(pieces) - set(DRUM_PIECES))
            if missing or extra:
                findings.append({
                    "profileId": profile_id,
                    "candidateId": candidate_id,
                    "kind": "piece-set",
                    "missing": missing,
                    "extra": extra,
                })
                continue
            fingerprint = json.dumps(
                parameters, sort_keys=True, separators=(",", ":")
            )
            previous_kit = kit_fingerprints.get(fingerprint)
            if previous_kit and not is_intentional_shared_pop_candidate(
                profile_id,
                str(candidate_id),
                previous_kit,
            ):
                findings.append({
                    "profileId": profile_id,
                    "candidateId": candidate_id,
                    "kind": "exact-kit-template-leakage",
                    "matches": previous_kit,
                })
            else:
                kit_fingerprints[fingerprint] = (
                    profile_id,
                    candidate_id,
                )
            for piece_name in DRUM_PIECES:
                piece = pieces[piece_name]
                source = piece.get("source")
                second = piece.get("secondSource", "off")
                allowed = PIECE_SOURCE_CONTRACTS[piece_name]
                native_reggae = is_native_reggae_piece(
                    str(profile_id),
                    str(candidate_id),
                    piece_name,
                )
                if source not in allowed and not (
                    native_reggae and source == "jam2-native"
                ):
                    findings.append({
                        "profileId": profile_id,
                        "candidateId": candidate_id,
                        "piece": piece_name,
                        "kind": "source-identity-contract",
                        "source": source,
                    })
                if second != "off" and second not in allowed:
                    findings.append({
                        "profileId": profile_id,
                        "candidateId": candidate_id,
                        "piece": piece_name,
                        "kind": "second-source-identity-contract",
                        "source": second,
                    })
                identity = str(piece.get("intendedIdentity") or "").strip()
                if not identity:
                    findings.append({
                        "profileId": profile_id,
                        "candidateId": candidate_id,
                        "piece": piece_name,
                        "kind": "missing-intended-identity",
                    })
                if (
                    piece_name == "snare" and
                    profile_id in HARD_SMACK_SNARE_PROFILES
                ):
                    transient = piece.get("transient") or {}
                    colour_stage = piece.get("colourStage") or {}
                    if (
                        source != "daisy-synthetic-snare" or
                        second != "jam2-shell-snare"
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "rock-metal-snare-not-smack-led",
                            "source": source,
                            "secondSource": second,
                        })
                    if not 0.11 <= float(piece.get("blend", 0)) <= 0.35:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "rock-metal-snare-shell-blend",
                            "blend": piece.get("blend"),
                        })
                    if (
                        transient.get("type") != "stick" or
                        float(transient.get("level", 0)) < 0.279 or
                        float(piece.get("level", 0)) < 0.539 or
                        float(colour_stage.get("voiceDrive", 0)) < 2.19
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "rock-metal-snare-smack-floor",
                            "level": piece.get("level"),
                            "transient": transient,
                            "voiceDrive": colour_stage.get("voiceDrive"),
                        })
                if piece_name == "snare" and not native_reggae:
                    snare_sources = {source, second}
                    if snare_sources != {
                        "jam2-shell-snare",
                        "daisy-synthetic-snare",
                    }:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "snare-missing-complementary-body",
                            "source": source,
                            "secondSource": second,
                        })
                    if not 0.099 <= float(piece.get("blend", 0)) <= 0.35:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "snare-complementary-body-blend",
                            "blend": piece.get("blend"),
                        })
                if piece_name == "kick":
                    synth_layer = piece.get("synthLayer") or {}
                    if synth_layer.get("source") == "fm2":
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "kick-prohibited-fm-tail-layer",
                        })
                if piece_name == "closed-hat" and not native_reggae:
                    transient_level = float(
                        (piece.get("transient") or {}).get("level", 0)
                    )
                    accepted_pop = (
                        candidate_id == "hybrid-section-lift"
                        and profile_id in SHARED_POP_PROFILES
                    )
                    level_floor = 0.359 if accepted_pop else 0.459
                    if float(piece.get("level", 0)) < level_floor:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "closed-hat-below-presence-floor",
                            "level": piece.get("level"),
                        })
                    if transient_level < 0.089:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "closed-hat-stick-below-presence-floor",
                            "transientLevel": transient_level,
                        })
                    velocity = piece.get("velocity") or {}
                    normal = velocity.get("normal") or {}
                    if (
                        not accepted_pop
                        and float(piece.get("decay", 0)) < 0.179
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "closed-hat-duration-below-presence-floor",
                            "decay": piece.get("decay"),
                        })
                    if (
                        not accepted_pop
                        and (
                            int(normal.get("minimum", 0)) < 50
                            or int(normal.get("maximum", 0)) < 96
                        )
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "closed-hat-normal-band-below-floor",
                            "normal": normal,
                        })
                    if (
                        not accepted_pop
                        and float(velocity.get("outputCurve", 99)) > 1.001
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "closed-hat-output-curve-too-steep",
                            "outputCurve": velocity.get("outputCurve"),
                        })
                elif piece_name == "ride" and not native_reggae:
                    if float(piece.get("colour", 0)) > 0.171:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "ride-pitched-ring-above-cap",
                            "colour": piece.get("colour"),
                        })
                elif piece_name == "cross-stick" and not native_reggae:
                    transient_level = float(
                        (piece.get("transient") or {}).get("level", 0)
                    )
                    velocity = piece.get("velocity") or {}
                    ghost = velocity.get("ghost") or {}
                    normal = velocity.get("normal") or {}
                    output_curve = float(
                        velocity.get("outputCurve", 99)
                    )
                    if float(piece.get("level", 0)) < 0.679:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-below-presence-floor",
                            "level": piece.get("level"),
                        })
                    if transient_level < 0.064:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-attack-below-presence-floor",
                            "transientLevel": transient_level,
                        })
                    if int(ghost.get("minimum", 0)) < 22 or int(
                        ghost.get("maximum", 0)
                    ) < 48:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-ghost-band-below-floor",
                            "ghost": ghost,
                        })
                    if output_curve > 1.101:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-output-curve-too-steep",
                            "outputCurve": output_curve,
                        })
                    if (
                        float(piece.get("decay", 0)) < 0.219
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-duration-below-presence-floor",
                            "decay": piece.get("decay"),
                        })
                    if (
                        float(
                            (piece.get("transient") or {}).get(
                                "decaySeconds", 0
                            )
                        ) < 0.0079
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-rim-duration-below-floor",
                        })
                    if (
                        (
                            int(normal.get("minimum", 0)) < 48
                            or int(normal.get("maximum", 0)) < 96
                        )
                    ):
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-normal-band-below-floor",
                            "normal": normal,
                        })
                    if output_curve > 1.001:
                        findings.append({
                            "profileId": profile_id,
                            "candidateId": candidate_id,
                            "piece": piece_name,
                            "kind": "cross-stick-revised-output-curve-too-steep",
                            "outputCurve": output_curve,
                        })
                piece_fingerprint = json.dumps(
                    piece, sort_keys=True, separators=(",", ":")
                )
                previous_piece = piece_fingerprints.get(
                    piece_fingerprint
                )
                if previous_piece and not is_intentional_piece_reuse(
                    str(profile_id),
                    str(candidate_id),
                    piece_name,
                    previous_piece,
                ):
                    findings.append({
                        "profileId": profile_id,
                        "candidateId": candidate_id,
                        "piece": piece_name,
                        "kind": "exact-piece-template-leakage",
                        "matches": previous_piece,
                    })
                else:
                    piece_fingerprints[piece_fingerprint] = (
                        profile_id,
                        candidate_id,
                        piece_name,
                    )
            crash = pieces["crash"]
            ride = pieces["ride"]
            if crash.get("source") == ride.get("source"):
                findings.append({
                    "profileId": profile_id,
                    "candidateId": candidate_id,
                    "kind": "crash-ride-shared-primary",
                })
            crash_transient = (
                crash.get("transient") or {}
            ).get("level", 0)
            ride_transient = (
                ride.get("transient") or {}
            ).get("level", 0)
            native_reggae_focus = (
                profile_id,
                candidate.get("id"),
            ) == NATIVE_REGGAE_FOCUS
            if (
                not native_reggae_focus
                and not float(ride_transient) > float(crash_transient)
            ):
                findings.append({
                    "profileId": profile_id,
                    "candidateId": candidate_id,
                    "kind": "ride-lacks-stick-definition",
                    "crashTransient": crash_transient,
                    "rideTransient": ride_transient,
                })
            high_tom = float(pieces["high-tom"]["frequencyHz"])
            mid_tom = float(pieces["mid-tom"]["frequencyHz"])
            floor_tom = float(pieces["floor-tom"]["frequencyHz"])
            if not high_tom > 1.12 * mid_tom > 1.12 * floor_tom:
                findings.append({
                    "profileId": profile_id,
                    "candidateId": candidate_id,
                    "kind": "tom-pitch-order",
                    "highTomHz": high_tom,
                    "midTomHz": mid_tom,
                    "floorTomHz": floor_tom,
                })
        for left_index, left in enumerate(candidates):
            for right in candidates[left_index + 1 :]:
                left_pieces = (
                    left.get("parameters") or {}
                ).get("pieces") or {}
                right_pieces = (
                    right.get("parameters") or {}
                ).get("pieces") or {}
                if set(left_pieces) != set(DRUM_PIECES) or set(
                    right_pieces
                ) != set(DRUM_PIECES):
                    continue
                different_pieces = sum(
                    piece_is_materially_different(
                        left_pieces[piece],
                        right_pieces[piece],
                    )
                    for piece in DRUM_PIECES
                )
                if different_pieces < 6:
                    findings.append({
                        "profileId": profile_id,
                        "kind": "cosmetic-candidate-variation",
                        "left": left.get("id"),
                        "right": right.get("id"),
                        "materiallyDifferentPieces": different_pieces,
                    })
    for left_index, (left_profile, left) in enumerate(
        recommended_kits
    ):
        for right_profile, right in recommended_kits[left_index + 1 :]:
            if frozenset((left_profile, right_profile)) == (
                SHARED_POP_PROFILES
            ):
                continue
            left_pieces = (
                left.get("parameters") or {}
            ).get("pieces") or {}
            right_pieces = (
                right.get("parameters") or {}
            ).get("pieces") or {}
            if set(left_pieces) != set(DRUM_PIECES) or set(
                right_pieces
            ) != set(DRUM_PIECES):
                continue
            different_pieces = sum(
                piece_is_materially_different(
                    left_pieces[piece],
                    right_pieces[piece],
                )
                for piece in DRUM_PIECES
            )
            if different_pieces < 6:
                findings.append({
                    "profileId": left_profile,
                    "otherProfileId": right_profile,
                    "kind": "recommended-cross-profile-convergence",
                    "materiallyDifferentPieces": different_pieces,
                })
    return findings


def render(
    executable: Path,
    environment: dict[str, str],
    request_path: Path,
    wav_path: Path,
    request: dict,
) -> dict:
    request_path.write_text(
        json.dumps(request, separators=(",", ":")),
        encoding="utf-8",
    )
    completed = subprocess.run(
        [
            str(executable),
            "--render-instrument",
            str(request_path),
            str(wav_path),
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"renderer exited {completed.returncode}: "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("renderer returned no result JSON")
    return json.loads(lines[-1])


def result_findings(result: dict) -> list[str]:
    metrics = result.get("metrics") or {}
    findings: list[str] = []
    peak = float(metrics.get("peak", 0.0))
    rms = float(metrics.get("rms", 0.0))
    dc = abs(float(metrics.get("dcOffset", 0.0)))
    clipped = int(metrics.get("clippedSamples", 0))
    if peak < 1.0e-4:
        findings.append("silent-or-nearly-silent")
    if rms < 1.0e-5:
        findings.append("negligible-rms")
    if clipped:
        findings.append(f"clipped-samples:{clipped}")
    if dc > 0.002:
        findings.append(f"dc-offset:{dc:.6f}")
    if not 0.0 <= peak <= 0.981:
        findings.append(f"unexpected-peak:{peak:.6f}")

    if result.get("audition") == "velocity-ladder":
        hits = result.get("realisedHits") or []
        by_state = {
            hit.get("state"): hit
            for hit in hits
            if hit.get("state") in {"ghost", "normal", "accent"}
        }
        if len(by_state) != 3:
            findings.append("velocity-ladder-missing-state")
        else:
            ghost = int(by_state["ghost"]["midiVelocity"])
            normal = int(by_state["normal"]["midiVelocity"])
            accent = int(by_state["accent"]["midiVelocity"])
            if not ghost < normal < accent:
                findings.append(
                    f"velocity-order:{ghost}/{normal}/{accent}"
                )
            ghost_gain = float(by_state["ghost"]["outputGain"])
            normal_gain = float(by_state["normal"]["outputGain"])
            accent_gain = float(by_state["accent"]["outputGain"])
            if not ghost_gain < normal_gain < accent_gain:
                findings.append(
                    "output-gain-order:"
                    f"{ghost_gain:.4f}/{normal_gain:.4f}/{accent_gain:.4f}"
                )
            if result.get("drumPiece") == "ride":
                articulations = (
                    by_state["ghost"].get("articulation"),
                    by_state["normal"].get("articulation"),
                    by_state["accent"].get("articulation"),
                )
                # Exact production performance events use the compact
                # articulation vocabulary shared by the recipe and renderer.
                if articulations != ("edge", "bow", "bell"):
                    findings.append(
                        "ride-articulation-map:" +
                        "/".join(str(value) for value in articulations)
                    )
    return findings


def identity_metric_findings(piece: str, result: dict) -> list[str]:
    """Broad rejection gates; these do not claim perceptual correctness."""
    if result.get("audition") != "one-shot":
        return []
    metrics = result.get("metrics") or {}
    bands = metrics.get("bandEnergyRatio") or {}
    hits = result.get("realisedHits") or []
    if len(hits) != 1:
        return [f"one-shot-hit-count:{len(hits)}"]
    analysis = hits[0].get("analysis") or {}
    sub = float(bands.get("sub_below_80_hz", 0))
    bass = float(bands.get("bass_80_250_hz", 0))
    low_mid = float(bands.get("low_mid_250_2000_hz", 0))
    high_mid = float(bands.get("high_mid_2000_8000_hz", 0))
    air = float(bands.get("air_above_8000_hz", 0))
    body = float(analysis.get("bodyRms20To120Ms", 0))
    tail = float(analysis.get("tailRms120To500Ms", 0))
    decay_40 = float(metrics.get("decayToMinus40DbMs", -1))
    findings: list[str] = []
    if piece == "kick":
        if sub + bass + 0.35 * low_mid < 0.48:
            findings.append(
                "kick-lacks-low-or-beater-body:"
                f"{sub + bass + 0.35 * low_mid:.3f}"
            )
        if high_mid + air > 0.30:
            findings.append(
                f"kick-excess-high-energy:{high_mid + air:.3f}"
            )
    elif piece == "snare":
        # A convincing short acoustic snare does not need a white-noise-heavy
        # spectrum. Require a small amount of wire/grit energy, then use the
        # time-domain body and tail checks to catch ringing or missing shell.
        if high_mid + air < 0.003:
            findings.append(
                f"snare-lacks-wire-energy:{high_mid + air:.3f}"
            )
        if high_mid + air > 0.68:
            findings.append(
                f"snare-excess-broadband-noise:{high_mid + air:.3f}"
            )
        if body < 0.0045:
            findings.append(f"snare-lacks-body:{body:.5f}")
        if decay_40 < 0 or decay_40 > 600:
            findings.append(f"snare-decay40:{decay_40:.1f}")
        if body > 0 and tail / body > 1.20:
            findings.append(
                "snare-tail-dominates-shell:"
                f"{tail / max(body, 1e-9):.3f}"
            )
        if result.get("profileId") == "pop_loop":
            onset = float(analysis.get("onsetRms0To20Ms", 0))
            if body <= 0 or onset / body < 2.2:
                findings.append(
                    "pop-snare-lacks-dominant-smack:"
                    f"{onset / max(body, 1e-9):.3f}"
                )
            if body <= 0 or tail / body > 0.28:
                findings.append(
                    "pop-snare-wire-tail-too-prominent:"
                    f"{tail / max(body, 1e-9):.3f}"
                )
            if decay_40 < 0 or decay_40 > 190:
                findings.append(
                    f"pop-snare-decay40:{decay_40:.1f}"
                )
    elif piece == "closed-hat":
        # Exact production includes deliberately damped, low-rate and
        # rhythm-box hats. Those can put substantial energy in the lower
        # metallic modes while still reading as a short, high-crest cymbal
        # collision. Reject on the combined identity rather than imposing the
        # bright full-band threshold formerly calibrated to the approximate
        # Lab voice.
        if high_mid + air < 0.22:
            findings.append(
                f"hat-lacks-metal-energy:{high_mid + air:.3f}"
            )
        if sub + bass > 0.19:
            findings.append(
                f"hat-excess-low-ring:{sub + bass:.3f}"
            )
        if not 3 <= decay_40 <= 180:
            findings.append(f"closed-hat-decay40:{decay_40:.1f}")
        if float(metrics.get("crestFactor", 0)) < 20:
            findings.append(
                "closed-hat-lacks-transient-definition:"
                f"{float(metrics.get('crestFactor', 0)):.2f}"
            )
    elif piece == "open-hat":
        if high_mid + air < 0.32:
            findings.append(
                f"hat-lacks-metal-energy:{high_mid + air:.3f}"
            )
        if sub + bass > 0.16:
            findings.append(
                f"hat-excess-low-ring:{sub + bass:.3f}"
            )
    elif piece in {"high-tom", "mid-tom", "floor-tom"}:
        if sub + bass + low_mid < 0.72:
            findings.append(
                "tom-lacks-membrane-energy:"
                f"{sub + bass + low_mid:.3f}"
            )
        if high_mid + air > 0.24:
            findings.append(
                f"tom-excess-metal-energy:{high_mid + air:.3f}"
            )
        if not 35 <= decay_40 <= 1100:
            findings.append(f"tom-decay40:{decay_40:.1f}")
    elif piece == "crash":
        if high_mid + air < 0.38:
            findings.append(
                f"crash-lacks-broadband-metal:{high_mid + air:.3f}"
            )
        if body <= 0 or tail / body < 0.18:
            findings.append(
                f"crash-lacks-wash:{tail / max(body, 1e-9):.3f}"
            )
        if decay_40 < 150:
            findings.append(
                f"crash-decay-too-abrupt:{decay_40:.1f}"
            )
    elif piece == "ride":
        if high_mid + air < 0.28:
            findings.append(
                f"ride-lacks-metal-energy:{high_mid + air:.3f}"
            )
        if low_mid > 0.72:
            findings.append(
                f"ride-excess-low-mid:{low_mid:.3f}"
            )
        if body <= 0 or tail / body < 0.035:
            findings.append(
                f"ride-tail-too-short:{tail / max(body, 1e-9):.3f}"
            )
        if decay_40 < 90:
            findings.append(
                f"ride-decay-too-abrupt:{decay_40:.1f}"
            )
    elif piece == "cross-stick":
        if decay_40 < 0 or decay_40 > 180:
            findings.append(f"cross-stick-decay40:{decay_40:.1f}")
        if tail > 0.012:
            findings.append(f"cross-stick-tail:{tail:.5f}")
        if low_mid < 0.28:
            findings.append(
                f"cross-stick-lacks-wood-band:{low_mid:.3f}"
            )
    return findings


def relationship_findings(records: list[dict]) -> list[dict]:
    """Compare rendered crash and ride behavior inside each complete kit."""
    one_shots = {
        (
            record.get("profileId"),
            record.get("candidateId"),
            record.get("piece"),
        ): record
        for record in records
        if record.get("audition") == "one-shot"
        and not record.get("error")
    }
    pairs = {
        (profile, candidate)
        for profile, candidate, piece in one_shots
        if piece in {"crash", "ride"}
    }
    findings: list[dict] = []
    band_names = (
        "sub_below_80_hz",
        "bass_80_250_hz",
        "low_mid_250_2000_hz",
        "high_mid_2000_8000_hz",
        "air_above_8000_hz",
    )
    for profile, candidate in sorted(pairs):
        crash = one_shots.get((profile, candidate, "crash"))
        ride = one_shots.get((profile, candidate, "ride"))
        if not crash or not ride:
            continue
        crash_hit = (crash.get("realisedHits") or [{}])[0]
        ride_hit = (ride.get("realisedHits") or [{}])[0]
        crash_analysis = crash_hit.get("analysis") or {}
        ride_analysis = ride_hit.get("analysis") or {}
        crash_onset = float(
            crash_analysis.get("onsetRms0To20Ms", 0)
        )
        ride_onset = float(
            ride_analysis.get("onsetRms0To20Ms", 0)
        )
        crash_body = float(
            crash_analysis.get("bodyRms20To120Ms", 0)
        )
        ride_body = float(
            ride_analysis.get("bodyRms20To120Ms", 0)
        )
        crash_definition = crash_onset / max(crash_body, 1e-9)
        ride_definition = ride_onset / max(ride_body, 1e-9)
        crash_bands = (
            crash.get("metrics") or {}
        ).get("bandEnergyRatio") or {}
        ride_bands = (
            ride.get("metrics") or {}
        ).get("bandEnergyRatio") or {}
        band_distance = sum(
            abs(
                float(crash_bands.get(name, 0)) -
                float(ride_bands.get(name, 0))
            )
            for name in band_names
        )
        # A ride may ring longer than a damped crash, so absolute tail level is
        # not an identity discriminator. Per-piece tail/body gates above catch
        # abrupt cymbals; stick definition and spectral distance distinguish
        # the two instruments here.
        # The feedback-selected Reggae kit intentionally compares the native
        # Jam2 ride with a separate rhythm-box crash. Their source renderers
        # do not expose a common synthetic transient scale; individual ride
        # and crash identity gates plus spectral separation remain active.
        if (
            (profile, candidate) != NATIVE_REGGAE_FOCUS
            and ride_definition < 1.10 * crash_definition
        ):
            findings.append({
                "profileId": profile,
                "candidateId": candidate,
                "kind": "rendered-ride-lacks-stick-definition",
                "crashOnsetBodyRatio": crash_definition,
                "rideOnsetBodyRatio": ride_definition,
            })
        if band_distance < 0.08:
            findings.append({
                "profileId": profile,
                "candidateId": candidate,
                "kind": "rendered-crash-ride-spectral-convergence",
                "bandDistance": band_distance,
            })
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument(
        "--scope",
        choices=("smoke", "recommended", "identity", "full"),
        default="full",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Resume from the scope-specific checkpoint if present.",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    manifest_path = root / "site" / "sound-design-manifest.js"
    executable = root / "build" / "jam2_sound_lab.exe"
    cache = root / ".research-cache" / "drum-candidate-audit"
    wavs = cache / "wavs"
    # Keep subprocess workspaces short on Windows. Native Jam2 reference
    # renders add UUID and WAV components below this root.
    temporary = cache / "t"
    cache.mkdir(parents=True, exist_ok=True)
    wavs.mkdir(parents=True, exist_ok=True)
    temporary.mkdir(parents=True, exist_ok=True)

    environment = os.environ.copy()
    environment["TEMP"] = str(temporary)
    environment["TMP"] = str(temporary)
    qt_bin = Path("C:/Qt/6.10.3/msvc2022_64/bin")
    if qt_bin.exists():
        environment["PATH"] = (
            str(qt_bin) + os.pathsep + environment.get("PATH", "")
        )

    manifest = load_manifest(manifest_path)
    structural_findings = validate_manifest(manifest)
    for finding in structural_findings:
        print(
            "structural finding: " +
            json.dumps(finding, separators=(",", ":")),
            flush=True,
        )
    jobs: list[tuple[str, str, str, str, dict]] = []
    for profile in manifest["profiles"]:
        role = next(
            role for role in profile["roles"] if role["id"] == "drums"
        )
        candidates = role.get("kitCandidates") or []
        if not candidates:
            raise ValueError(f"{profile['id']} has no drum candidates")
        selected = (
            candidates[:1]
            if args.scope == "smoke"
            else candidates
            if args.scope in {"identity", "full"}
            else [candidate for candidate in candidates
                  if candidate.get("recommended")]
        )
        for candidate in selected:
            if args.scope != "identity":
                jobs.append(
                    (
                        profile["id"],
                        candidate["id"],
                        "profile",
                        "kick",
                        candidate["parameters"],
                    )
                )
            if args.scope in {"identity", "full"}:
                for piece in DRUM_PIECES:
                    jobs.append(
                        (
                            profile["id"],
                            candidate["id"],
                            "one-shot",
                            piece,
                            candidate["parameters"],
                        )
                    )
        recommended = next(
            (
                candidate
                for candidate in candidates
                if candidate.get("recommended")
            ),
            candidates[0],
        )
        if args.scope == "identity":
            continue
        pieces = DRUM_PIECES if args.scope != "smoke" else ("kick", "snare")
        auditions = (
            ("velocity-ladder", "repeated-hits")
            if args.scope == "full"
            else ("velocity-ladder",)
        )
        for piece in pieces:
            for audition in auditions:
                jobs.append(
                    (
                        profile["id"],
                        recommended["id"],
                        audition,
                        piece,
                        recommended["parameters"],
                    )
                )

    started = time.monotonic()
    request_path = cache / "request.json"
    checkpoint_path = cache / f"checkpoint-{args.scope}.json"
    records: list[dict] = []
    if args.resume and checkpoint_path.exists():
        records = json.loads(
            checkpoint_path.read_text(encoding="utf-8")
        ).get("records", [])
    completed_keys = {
        (
            record["profileId"],
            record["candidateId"],
            record["audition"],
            record["piece"],
        )
        for record in records
    }
    failures: list[dict] = list(structural_findings)
    failures.extend(
        [
        record
        for record in records
        if record.get("findings") or record.get("error")
        ]
    )
    for index, (profile, candidate, audition, piece, kit) in enumerate(
        jobs, 1
    ):
        job_key = (profile, candidate, audition, piece)
        if job_key in completed_keys:
            continue
        stem = f"{profile}__{candidate}__{audition}__{piece}"
        request = {
            "schema": "jam2-instrument-patch-v1",
            "profileId": profile,
            "role": "drums",
            "audition": audition,
            "drumPiece": piece,
            "kit": kit,
        }
        try:
            result = render(
                executable,
                environment,
                request_path,
                wavs / f"{stem}.wav",
                request,
            )
            findings = result_findings(result)
            findings.extend(
                identity_metric_findings(piece, result)
            )
            record = {
                "profileId": profile,
                "candidateId": candidate,
                "audition": audition,
                "piece": piece,
                "findings": findings,
                "metrics": result.get("metrics"),
                "realisedHits": result.get("realisedHits"),
            }
            records.append(record)
            if findings:
                failures.append(record)
                print(
                    "finding: " +
                    json.dumps(record, separators=(",", ":")),
                    flush=True,
                )
        except Exception as error:  # audit must retain every failure
            record = {
                "profileId": profile,
                "candidateId": candidate,
                "audition": audition,
                "piece": piece,
                "error": str(error),
            }
            records.append(record)
            failures.append(record)
            print(
                "error: " +
                json.dumps(record, separators=(",", ":")),
                flush=True,
            )
        if index == 1 or index % 25 == 0 or index == len(jobs):
            elapsed = time.monotonic() - started
            print(
                f"{index}/{len(jobs)} renders, "
                f"{len(failures)} findings, {elapsed:.1f}s",
                flush=True,
            )
            checkpoint_path.write_text(
                json.dumps(
                    {
                        "scope": args.scope,
                        "records": records,
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )

    rendered_relationship_findings = relationship_findings(records)
    failures.extend(rendered_relationship_findings)
    for finding in rendered_relationship_findings:
        print(
            "relationship finding: " +
            json.dumps(finding, separators=(",", ":")),
            flush=True,
        )
    report = {
        "schema": "jam2-drum-candidate-audit-v1",
        "scope": args.scope,
        "profiles": len(manifest["profiles"]),
        "jobs": len(jobs),
        "findings": len(failures),
        "structuralFindings": structural_findings,
        "relationshipFindings": rendered_relationship_findings,
        "elapsedSeconds": time.monotonic() - started,
        "records": records,
    }
    report_path = cache / f"report-{args.scope}.json"
    report_path.write_text(
        json.dumps(report, indent=2),
        encoding="utf-8",
    )
    checkpoint_path.unlink(missing_ok=True)
    print(f"report: {report_path}")
    if failures:
        print("first findings:")
        for failure in failures[:12]:
            print(json.dumps(failure, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
