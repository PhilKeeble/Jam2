#!/usr/bin/env python3

import argparse
import json
import math
import secrets
import shutil
import wave
import threading
import time
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_harness import (
    collect_side_csv,
    ManagedProcess,
    parse_endpoint,
    rewrite_jam_url_endpoint,
    side_paths,
    start_connector,
    start_listener,
)
from jam2_metrics import combined_summary, summarize_csv, write_results_csv
from jam2_profiles import (
    FAST_PROFILE,
    MODERATE_PROFILE,
    SAFE_PROFILE,
    adaptive_off_profile,
    jitter_buffer_profile,
    latency_matched_prefill_profile,
    variant,
)
from jam2_tooling import default_jam2_path, ensure_dir, fail, print_flush, repo_root, safe_test_id, write_json
from udp_stress_proxy import DirectionImpairment, ProxyImpairment, UdpStressProxy


DEFAULT_STREAM_MS = 30000
METRONOME_WAV_TOLERANCE_FRAMES = 96


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run localhost Jam2 stress scenarios through a UDP impairment proxy or headless mesh peers.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--mode", choices=("normal", "mesh"), default="normal")
    parser.add_argument("--server-audio-device")
    parser.add_argument("--client-audio-device")
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("stress_logs")))
    parser.add_argument("--stream-ms", type=int, default=DEFAULT_STREAM_MS)
    parser.add_argument("--startup-timeout-s", type=float, default=10.0)
    parser.add_argument("--clean", action="store_true", help="delete the stress log directory before running")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument(
        "--profile",
        choices=("fast", "moderate", "safe", "all"),
        default="fast",
        help="profile family for stress scenarios; all duplicates selected scenarios across fast, moderate, and safe")
    parser.add_argument("--include-validation", action="store_true", help="also run short CLI/session/error validation checks")
    parser.add_argument(
        "--include-audio-probes",
        action="store_true",
        help="also run targeted recorded tone/pulse probes for audible artifact analysis")
    parser.add_argument("--validation-stream-ms", type=int, default=5000)
    parser.add_argument(
        "--scenario",
        action="append",
        default=[],
        help="scenario id to run; repeat for multiple. Default runs the standard suite.")
    parser.add_argument(
        "--os-priority",
        action="append",
        choices=("off", "high", "realtime", "all"),
        default=None,
        help="OS priority mode to run; repeat for multiple or use all. Default: high.")
    parser.add_argument(
        "--mesh-peers",
        action="append",
        type=int,
        default=None,
        help="mesh peer count to run in --mode mesh; repeat for multiple. Default: 2, 3, 4, 8.")
    parser.add_argument(
        "--mesh-base-port",
        type=int,
        default=50000,
        help="first localhost UDP port used by headless mesh stress cases")
    return parser.parse_args()


def selected_profile(profile_name):
    if profile_name == "fast":
        return FAST_PROFILE
    if profile_name == "moderate":
        return MODERATE_PROFILE
    if profile_name == "safe":
        return SAFE_PROFILE
    raise ValueError(f"unknown profile: {profile_name}")


def selected_os_priorities(values):
    if not values:
        return ["high"]
    if "all" in values:
        return ["off", "high", "realtime"]
    priorities = []
    for value in values:
        if value not in priorities:
            priorities.append(value)
    return priorities


def audio_probe_health_ok(analysis):
    return analysis.get("ok", False) and not analysis.get("tags", [])


def scenario_catalog(base_profile=FAST_PROFILE):
    return {
        "clean-control": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "no injected impairment",
        },
        "clean-fast-control": {
            "profile": FAST_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "fast localhost baseline with no injected impairment",
        },
        "clean-moderate-control": {
            "profile": MODERATE_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "moderate localhost baseline with no injected impairment",
        },
        "clean-safe-control": {
            "profile": SAFE_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "safe localhost baseline with no injected impairment",
        },
        "jitter-20": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "ordered jitter should raise jitter/RTT without proxy packet loss",
        },
        "jitter-50": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "expect": "high ordered jitter should show in jitter/RTT stats and pressure playback",
        },
        "jitter-100": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=100.0)),
            "expect": "extreme ordered jitter should pressure playback depth and adaptive cushion",
        },
        "burst-pause-250": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "short periodic stalls should recover without large lasting damage",
        },
        "burst-pause-500": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=500.0, burst_every_ms=8000.0)),
            "expect": "medium stalls should create visible cushion/underrun pressure",
        },
        "burst-pause-1500": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=1500.0, burst_every_ms=10000.0)),
            "expect": "long stalls should cause obvious underruns but recover afterwards",
        },
        "loss-0.1": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=0.1)),
            "expect": "low loss should appear in sequence loss with limited playback impact",
        },
        "loss-0.5": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=0.5)),
            "expect": "moderate loss should be measurable and may insert missing audio",
        },
        "loss-1.0": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "expect": "high loss should create clear missing-frame diagnostics",
        },
        "reorder-small": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=8.0, reorder_percent=2.0, preserve_order=False)),
            "expect": "small reordering should show recovery/loss counters",
        },
        "adaptive-on-pressure": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "comparison run for adaptive cushion enabled under pressure",
        },
        "adaptive-off-pressure": {
            "profile": adaptive_off_profile(base_profile),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "comparison run for adaptive cushion disabled under pressure",
        },
        "jitter-buffer-512-pressure": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "jitter buffer uses most of the latency budget while adaptive cushion remains available under pressure",
        },
        "jitter-buffer-512-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "same jitter target as the failing pressure case with larger jitter-buffer headroom",
        },
        "jitter-buffer-1024-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=1024, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "larger jitter target and max under the same pressure case",
        },
        "jitter-buffer-2048-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "safe-style jitter target under the same pressure case with aggressive playback tail",
        },
        "jitter-buffer-2048-max4096-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
                "max_4096",
                jitter_buffer_max_frames=4096),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "safe-style jitter target with enough jitter-buffer headroom for observed pressure depth",
        },
        "prefill-768-pressure": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "latency-matched prefill comparison for jitter-buffer-512-pressure",
        },
        "jitter-buffer-512-adaptive-off-pressure": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=False),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "fixed jitter buffer without adaptive cushion under pressure",
        },
        "prefill-768-adaptive-off-pressure": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=False),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "fixed prefill without adaptive cushion under pressure",
        },
        "jitter-buffer-1024-jitter-100": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=1024, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=100.0)),
            "expect": "larger jitter buffer should reduce underrun pressure under extreme ordered jitter at the cost of latency",
        },
        "jitter-buffer-512-reorder-small": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=8.0, reorder_percent=2.0, preserve_order=False)),
            "expect": "jitter buffer should work with the reorder path and expose queued/released counters",
        },
        "metronome-shared-grid": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "metronome packets should be exchanged and final alignment should stay valid",
        },
        "metronome-leader-audio": {
            "profile": variant(base_profile, "metro_leader_audio", metronome_mode="leader-audio"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "leader-audio metronome mode should run and exchange metronome state",
        },
        "metronome-listener-compensated": {
            "profile": variant(base_profile, "metro_listener_compensated", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "listener-compensated metronome mode should keep the shared epoch valid while applying local render compensation",
        },
        "metronome-listener-compensated-metro-pulse": {
            "profile": variant(base_profile, "metro_listener_compensated_metro_pulse", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "metro-pulse",
            "server_signal": "metro-pulse",
            "client_signal": "metro-pulse",
            "expect": "listener-compensated metronome should align local compensated clicks to incoming peer pulses generated from the peer metronome epoch",
        },
        "metronome-listener-compensated-pulse-jitter": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should track incoming peer pulse timing under ordered jitter",
        },
        "metronome-listener-compensated-pulse-burst": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse_burst", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should expose pulse/click tracking under burst pressure",
        },
        "metronome-listener-compensated-pulse-loss": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse_loss", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0, jitter_ms=20.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should report pulse/click tracking when packet loss creates missing remote pulses",
        },
        "levels-low": {
            "profile": variant(base_profile, "levels_low", metronome_level=0.05, remote_level=0.50),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "startup metronome and remote levels should be reflected in final CSV state",
        },
        "sample-time-playout-off": {
            "profile": variant(base_profile, "sample_time_off", sample_time_playout="off"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "non-sample-time playout mode should still stream cleanly and report the requested mode",
        },
        "playout-delay-3072": {
            "profile": variant(
                base_profile,
                "playout_delay_3072",
                playback_max_frames=4096,
                playout_delay_frames=3072,
                adaptive_playback_target_frames=3072,
                adaptive_playback_min_frames=3072,
                adaptive_playback_max_frames=max(4096, base_profile.adaptive_playback_max_frames)),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "larger playout delay should be applied and visible in depth/latency metrics",
        },
        "drift-max-5ppm": {
            "profile": variant(base_profile, "drift_max_5ppm", drift_deadband_ppm=0, drift_max_correction_ppm=5),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "drift correction should respect a very low max correction cap when real device drift exceeds it",
        },
        "drift-smoothing-fast": {
            "profile": variant(base_profile, "drift_smoothing_fast", drift_smoothing=1.0, drift_deadband_ppm=0),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "fast drift smoothing should run and report drift/resampler data",
        },
        "socket-buffers": {
            "profile": variant(base_profile, "socket_buffers", socket_send_buffer=1048576, socket_recv_buffer=1048576),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "expect": "requested socket buffers should be applied or reported with the OS-adjusted actual values",
        },
        "channels-1-to-1": {
            "profile": variant(base_profile, "channels_1_to_1", input_channels="1", output_channels="1"),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "explicit channel selection should run and be visible in CSV metadata",
        },
        "runtime-controls": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "commands": [
                {"at_s": 3.0, "side": "server", "line": "metro off"},
                {"at_s": 5.0, "side": "server", "line": "metro on"},
                {"at_s": 7.0, "side": "server", "line": "metro mode listener-compensated"},
                {"at_s": 9.0, "side": "client", "line": "metro level 0.10"},
                {"at_s": 11.0, "side": "client", "line": "remote level 0.75"},
                {"at_s": 13.0, "side": "server", "line": "stats"},
            ],
            "expect": "runtime command path should update final levels/mode while audio continues",
        },
    }


def standard_suite():
    return [
        "clean-control",
        "jitter-20",
        "jitter-50",
        "jitter-100",
        "burst-pause-250",
        "burst-pause-500",
        "burst-pause-1500",
        "loss-0.1",
        "loss-0.5",
        "loss-1.0",
        "reorder-small",
        "adaptive-on-pressure",
        "adaptive-off-pressure",
        "jitter-buffer-512-pressure",
        "jitter-buffer-512-max3072-pressure",
        "jitter-buffer-1024-max3072-pressure",
        "jitter-buffer-2048-max3072-pressure",
        "jitter-buffer-2048-max4096-pressure",
        "prefill-768-pressure",
        "jitter-buffer-512-adaptive-off-pressure",
        "prefill-768-adaptive-off-pressure",
        "jitter-buffer-1024-jitter-100",
        "jitter-buffer-512-reorder-small",
        "metronome-shared-grid",
        "metronome-leader-audio",
        "metronome-listener-compensated",
        "metronome-listener-compensated-metro-pulse",
        "metronome-listener-compensated-pulse-jitter",
        "metronome-listener-compensated-pulse-burst",
        "metronome-listener-compensated-pulse-loss",
        "levels-low",
        "sample-time-playout-off",
        "playout-delay-3072",
        "drift-max-5ppm",
        "drift-smoothing-fast",
        "socket-buffers",
        "channels-1-to-1",
        "runtime-controls",
    ]


def audio_probe_suite(base_profile=FAST_PROFILE):
    return {
        "audio-probe-clean-tone": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "symmetric tone should record cleanly without injected impairment",
        },
        "audio-probe-jitter-tone": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "symmetric tone should expose audible artifacts under high ordered jitter",
        },
        "audio-probe-loss-server-to-client": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "signal": "tone-server-to-client",
            "server_signal": "tone-440",
            "client_signal": "silence",
            "expect": "server-to-client tone should expose directional loss artifacts",
        },
        "audio-probe-loss-client-to-server": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "signal": "tone-client-to-server",
            "server_signal": "silence",
            "client_signal": "tone-440",
            "expect": "client-to-server tone should expose directional loss artifacts",
        },
        "audio-probe-adaptive-on-pulse": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "pulse-1s",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "pulse recording should show whether adaptive cushion masks burst pressure",
        },
        "audio-probe-adaptive-off-pulse": {
            "profile": adaptive_off_profile(base_profile),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "pulse-1s",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "pulse recording should expose the same burst pressure with adaptive cushion disabled",
        },
        "audio-probe-jitter-buffer-512-tone": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "jitter-buffer tone recording should expose audible artifacts and jitter-buffer stats under high ordered jitter",
        },
        "audio-probe-prefill-768-tone": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "latency-matched prefill tone recording comparison for jitter-buffer tone analysis",
        },
        "audio-probe-jitter-buffer-512-metronome": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "metronome-only",
            "server_signal": "silence",
            "client_signal": "silence",
            "expect": "jitter-buffer metronome recording should expose click timing error under Wi-Fi-like pressure",
        },
        "audio-probe-prefill-768-metronome": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "metronome-only",
            "server_signal": "silence",
            "client_signal": "silence",
            "expect": "latency-matched prefill metronome recording comparison for jitter-buffer timing analysis",
        },
    }


def expand_scenarios(scenario_ids):
    expanded = []
    for scenario_id in scenario_ids:
        if scenario_id == "adaptive-off-vs-on":
            expanded.extend(["adaptive-on-pressure", "adaptive-off-pressure"])
        elif scenario_id == "burst-loss":
            expanded.extend(["burst-pause-500", "loss-1.0"])
        elif scenario_id == "jitter-buffer-adaptive-pressure-matrix":
            expanded.extend([
                "jitter-buffer-512-pressure",
                "jitter-buffer-512-max3072-pressure",
                "jitter-buffer-1024-max3072-pressure",
                "jitter-buffer-2048-max3072-pressure",
                "jitter-buffer-2048-max4096-pressure",
                "jitter-buffer-512-adaptive-off-pressure",
                "prefill-768-pressure",
            ])
        elif scenario_id == "jitter-buffer-audio-vs-prefill":
            expanded.extend([
                "audio-probe-jitter-buffer-512-tone",
                "audio-probe-prefill-768-tone",
                "audio-probe-jitter-buffer-512-metronome",
                "audio-probe-prefill-768-metronome",
            ])
        else:
            expanded.append(scenario_id)
    return expanded


def scenario_plan(profile_mode, requested_scenarios, include_audio_probes=False):
    base_ids = expand_scenarios(requested_scenarios) if requested_scenarios else standard_suite()
    if profile_mode != "all":
        base_profile = selected_profile(profile_mode)
        catalog = scenario_catalog(base_profile)
        needs_audio_probes = include_audio_probes or any(scenario_id.startswith("audio-probe-") for scenario_id in base_ids)
        if needs_audio_probes:
            probes = audio_probe_suite(base_profile)
            catalog.update(probes)
            if include_audio_probes:
                base_ids = base_ids + list(probes.keys())
        return catalog, base_ids

    planned_catalog = {}
    planned_ids = []
    for profile_name in ("fast", "moderate", "safe"):
        base_profile = selected_profile(profile_name)
        catalog = scenario_catalog(base_profile)
        profile_ids = list(base_ids)
        needs_audio_probes = include_audio_probes or any(scenario_id.startswith("audio-probe-") for scenario_id in profile_ids)
        if needs_audio_probes:
            probes = audio_probe_suite(base_profile)
            catalog.update(probes)
            if include_audio_probes:
                profile_ids.extend(probes.keys())
        for scenario_id in profile_ids:
            planned_id = f"{profile_name}__{scenario_id}"
            scenario = dict(catalog[scenario_id])
            scenario["source_scenario"] = scenario_id
            scenario["profile_family"] = profile_name
            planned_catalog[planned_id] = scenario
            planned_ids.append(planned_id)
    return planned_catalog, planned_ids


def mesh_peer_counts(args_ns):
    counts = args_ns.mesh_peers or [2, 3, 4, 8]
    out = []
    for count in counts:
        if count < 2:
            raise ValueError("--mesh-peers must be at least 2")
        if count not in out:
            out.append(count)
    return out


def mesh_scenario_catalog(base_profile, counts):
    return {
        f"mesh-{count}-clean": {
            "profile": base_profile,
            "mesh_peers": count,
            "signal": "tone-440",
            "expect": f"headless full mesh with {count} peers and no injected network impairment",
        }
        for count in counts
    }


def mesh_scenario_plan(profile_mode, requested_scenarios, counts):
    base_ids = requested_scenarios if requested_scenarios else [f"mesh-{count}-clean" for count in counts]
    if profile_mode != "all":
        catalog = mesh_scenario_catalog(selected_profile(profile_mode), counts)
        return catalog, base_ids

    planned_catalog = {}
    planned_ids = []
    for profile_name in ("fast", "moderate", "safe"):
        catalog = mesh_scenario_catalog(selected_profile(profile_name), counts)
        for scenario_id in base_ids:
            if scenario_id not in catalog:
                planned_ids.append(f"{profile_name}__{scenario_id}")
                continue
            planned_id = f"{profile_name}__{scenario_id}"
            scenario = dict(catalog[scenario_id])
            scenario["source_scenario"] = scenario_id
            scenario["profile_family"] = profile_name
            planned_catalog[planned_id] = scenario
            planned_ids.append(planned_id)
    return planned_catalog, planned_ids


def mesh_collect_metrics(peer_results):
    peers_with_csv = [peer for peer in peer_results if peer.get("csv_summary", {}).get("has_csv")]
    audio_tags = []
    for peer in peer_results:
        audio = peer.get("audio_analysis", {})
        audio_tags.extend(audio.get("tags", []))
    summaries = [peer["csv_summary"] for peer in peers_with_csv]
    return {
        "has_csv": bool(summaries),
        "peer_count": len(peer_results),
        "peers_with_csv": len(peers_with_csv),
        "return_code_failures": sum(1 for peer in peer_results if peer.get("return_code") != 0),
        "sent_packets_min": min((row.get("sent_packets", 0.0) for row in summaries), default=0.0),
        "recv_packets_min": min((row.get("recv_packets", 0.0) for row in summaries), default=0.0),
        "sent_packets_total": sum((row.get("sent_packets", 0.0) for row in summaries), 0.0),
        "recv_packets_total": sum((row.get("recv_packets", 0.0) for row in summaries), 0.0),
        "sequence_lost_total": sum((row.get("sequence_lost", 0.0) for row in summaries), 0.0),
        "sequence_loss_percent_max": max((row.get("sequence_loss_percent", 0.0) for row in summaries), default=0.0),
        "sequence_out_of_order_total": sum((row.get("sequence_out_of_order", 0.0) for row in summaries), 0.0),
        "sequence_duplicate_total": sum((row.get("sequence_duplicate", 0.0) for row in summaries), 0.0),
        "jitter_max_ms": max((row.get("jitter_max_ms", 0.0) for row in summaries), default=0.0),
        "rtt_max_ms": max((row.get("rtt_max_ms", 0.0) for row in summaries), default=0.0),
        "playback_underrun_time_ms_total": sum((row.get("playback_ring_underrun_time_ms", 0.0) for row in summaries), 0.0),
        "audio_ok_peers": sum(1 for peer in peer_results if peer.get("audio_analysis", {}).get("ok", False)),
        "audio_tags": sorted(set(audio_tags)),
    }


def mesh_verdict(result):
    metrics = result.get("mesh_metrics", {})
    peer_count = metrics.get("peer_count", 0)
    if metrics.get("return_code_failures", 0) > 0:
        return "process_failed"
    if metrics.get("peers_with_csv", 0) != peer_count:
        return "missing_csv"
    if metrics.get("sent_packets_min", 0.0) <= 0.0 or metrics.get("recv_packets_min", 0.0) <= 0.0:
        return "mesh_packets_missing"
    if metrics.get("sequence_lost_total", 0.0) > 0.0:
        return "unexpected_loss"
    if metrics.get("sequence_out_of_order_total", 0.0) > 0.0:
        return "unexpected_reorder"
    if metrics.get("audio_ok_peers", 0) != peer_count:
        return "mesh_audio_probe_failed"
    return "pass"


def run_mesh_scenario(jam2, scenario_id, scenario, args_ns, output_dir, seed):
    del seed
    profile = scenario["profile"]
    peer_count = scenario["mesh_peers"]
    os_priority = scenario.get("os_priority", "high")
    base_port = args_ns.mesh_base_port
    session_id = secrets.token_hex(8)
    session_key = secrets.token_hex(16)
    endpoints = [f"127.0.0.1:{base_port + index}" for index in range(peer_count)]
    processes = []
    peer_results = []
    print_flush(f"[stress] starting {scenario_id} profile={profile.name} peers={peer_count} os_priority={os_priority}")
    for index, endpoint in enumerate(endpoints):
        peer_name = f"peer{index + 1}"
        paths = side_paths(output_dir, peer_name)
        recording = ensure_dir(Path(paths["dir"]) / "recording")
        peers = ",".join(peer for peer in endpoints if peer != endpoint)
        args = [
            jam2,
            "mesh",
            "--bind", endpoint,
            "--session-id", session_id,
            "--session-key", session_key,
            "--peers", peers,
            "--sample-rate", str(args_ns.sample_rate),
            "--log-stats", str(paths["csv_raw"]),
            "--headless-audio", "on",
            "--test-input", scenario.get("signal", "tone-440"),
            "--record-jam-folder", str(recording),
            "--os-priority", os_priority,
            "--status-format", "jsonl",
        ]
        args.extend(profile.args(args_ns.stream_ms))
        process = ManagedProcess(
            args,
            repo_root(),
            paths["stdout"],
            paths["stderr"]).start()
        processes.append((process, paths, peer_name))

    timeout = max(30.0, args_ns.stream_ms / 1000.0 + 30.0)
    for process, paths, peer_name in processes:
        rc = process.wait(timeout=timeout)
        if rc is None:
            process.terminate()
            rc = process.poll()
        csv_path = collect_side_csv(paths)
        csv_summary = summarize_csv(csv_path) if csv_path else {"has_csv": False}
        audio_analysis = analyze_recording_dir(
            Path(paths["dir"]) / "recording",
            scenario.get("signal", "tone-440"),
            local_signal=scenario.get("signal", "tone-440"),
            remote_signal=scenario.get("signal", "tone-440"),
            ignore_pop_events=True,
            ignore_tone_frequency=True,
            ignore_inputs_mix_clipping=True)
        peer_results.append({
            "peer": peer_name,
            "return_code": rc,
            "csv_path": str(csv_path) if csv_path else "",
            "csv_summary": csv_summary,
            "audio_analysis": audio_analysis,
            "stdout": str(paths["stdout"]),
            "stderr": str(paths["stderr"]),
        })

    mesh_metrics = mesh_collect_metrics(peer_results)
    result = {
        "scenario": scenario_id,
        "source_scenario": scenario.get("source_scenario", scenario_id),
        "profile_family": scenario.get("profile_family", args_ns.profile),
        "profile": profile.name,
        "os_priority": os_priority,
        "expect": scenario["expect"],
        "profile_metadata": profile.metadata(),
        "mesh_peers": peer_count,
        "mesh_endpoints": endpoints,
        "peer_results": peer_results,
        "mesh_metrics": mesh_metrics,
    }
    result["verdict"] = mesh_verdict(result)
    print_flush(
        f"[stress] finished {scenario_id} verdict={result['verdict']} "
        f"peers={peer_count} recv_min={mesh_metrics.get('recv_packets_min', 0.0):.0f} "
        f"loss={mesh_metrics.get('sequence_lost_total', 0.0):.0f} "
        f"audio_ok={mesh_metrics.get('audio_ok_peers', 0)}/{peer_count}")
    return result


def verdict_for(result):
    metrics = result.get("metrics", {}).get("combined", {})
    proxy = result.get("proxy_stats", {})
    if result.get("server_return_code") != 0 or result.get("client_return_code") != 0:
        return "process_failed"
    if not metrics.get("has_csv"):
        return "missing_csv"
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario == "clean-control":
        if metrics.get("loss_percent_max", 0.0) > 0.0:
            return "unexpected_loss"
        if metrics.get("playback_underrun_time_ms_total", 0.0) > 20.0:
            return "unexpected_underrun"
        return "pass"
    if scenario.startswith("loss-"):
        injected = proxy.get("client_to_server_dropped", 0) + proxy.get("server_to_client_dropped", 0)
        if injected <= 0:
            return "no_proxy_loss_injected"
        if metrics.get("loss_percent_max", 0.0) <= 0.0 and metrics.get("missing_audio_frames_total", 0.0) <= 0.0:
            return "loss_not_observed"
        return "pass"
    if scenario.startswith("jitter-"):
        if metrics.get("jitter_max_ms", 0.0) <= 0.0 and metrics.get("rtt_max_ms", 0.0) <= 0.0:
            return "impairment_not_visible"
        if proxy.get("client_to_server_dropped", 0) + proxy.get("server_to_client_dropped", 0) > 0:
            return "unexpected_proxy_drop"
        return "pass"
    if scenario.startswith("burst-pause-"):
        blackouts = proxy.get("client_to_server_blackout_events", 0) + proxy.get("server_to_client_blackout_events", 0)
        if blackouts <= 0:
            return "burst_blackout_not_injected"
        if (
                metrics.get("playback_underrun_time_ms_total", 0.0) <= 0.0
                and metrics.get("missing_audio_frames_total", 0.0) <= 0.0
                and metrics.get("sequence_lost_total", 0.0) <= 0.0):
            return "burst_not_observed"
        return "pass"
    if scenario == "reorder-small":
        if proxy.get("client_to_server_reordered", 0) + proxy.get("server_to_client_reordered", 0) <= 0:
            return "proxy_reorder_not_injected"
        if metrics.get("sequence_out_of_order_total", 0.0) <= 0.0 and metrics.get("reordered_recovered_total", 0.0) <= 0.0:
            return "reorder_not_observed"
        return "pass"
    if scenario.startswith("jitter-buffer-"):
        if metrics.get("jitter_buffer_released_packets_total", 0.0) <= 0.0:
            return "jitter_buffer_not_used"
        if scenario.endswith("reorder-small"):
            if proxy.get("client_to_server_reordered", 0) + proxy.get("server_to_client_reordered", 0) <= 0:
                return "proxy_reorder_not_injected"
        return "pass"
    if scenario == "metronome-shared-grid":
        return metronome_verdict(result, "shared-grid")
    if is_listener_compensated_pulse_tracking_scenario(scenario):
        base = metronome_verdict(result, "listener-compensated")
        if base != "pass":
            return base
        if scenario == "metronome-listener-compensated-metro-pulse":
            metro_pulse = result.get("metro_pulse_epoch_analysis", {})
            if not metro_pulse:
                return "metro_pulse_epoch_analysis_missing"
            if not metro_pulse.get("ok", False):
                return metro_pulse.get("verdict", "metro_pulse_epoch_failed")
        pulse = result.get("listener_compensated_pulse_analysis", {})
        if not pulse:
            return "listener_compensated_pulse_analysis_missing"
        if not pulse.get("ok", False):
            return pulse.get("verdict", "listener_compensated_pulse_failed")
        if pulse.get("combined", {}).get("matched_pulses_min", 0) <= 0:
            return "listener_compensated_pulse_not_matched"
        return "pass"
    if scenario.startswith("metronome-"):
        expected = scenario.removeprefix("metronome-")
        return metronome_verdict(result, expected)
    if scenario == "levels-low":
        server = result.get("metrics", {}).get("server", {})
        client = result.get("metrics", {}).get("client", {})
        if abs(server.get("final_metronome_level", 0.0) - 0.05) > 0.01:
            return "metronome_level_not_applied"
        if abs(client.get("final_remote_level", 0.0) - 0.50) > 0.01:
            return "remote_level_not_applied"
        return "pass"
    if scenario == "sample-time-playout-off":
        if result.get("metrics", {}).get("server", {}).get("sample_time_playout") != "off":
            return "sample_time_playout_not_off"
        return "pass"
    if scenario == "playout-delay-3072":
        if result.get("metrics", {}).get("server", {}).get("playout_delay_frames", 0.0) != 3072.0:
            return "playout_delay_not_applied"
        return "pass"
    if scenario == "drift-max-5ppm":
        ratio_min = metrics.get("resampler_ratio_min", 1.0)
        ratio_max = metrics.get("resampler_ratio_max", 1.0)
        if ratio_min < 0.999990 or ratio_max > 1.000010:
            return "resampler_exceeded_expected_cap"
        return "pass"
    if scenario == "socket-buffers":
        server = result.get("metrics", {}).get("server", {})
        if server.get("requested_socket_send_buffer_bytes", 0.0) <= 0.0:
            return "socket_buffers_not_requested"
        if server.get("socket_send_buffer_bytes", 0.0) <= 0.0 or server.get("socket_recv_buffer_bytes", 0.0) <= 0.0:
            return "socket_buffers_not_reported"
        return "pass"
    if scenario == "channels-1-to-1":
        server = result.get("metrics", {}).get("server", {})
        if "input=1" not in server.get("requested_channels", ""):
            return "input_channels_not_reported"
        if "output=1" not in server.get("requested_channels", ""):
            return "output_channels_not_reported"
        return "pass"
    if scenario == "runtime-controls":
        client = result.get("metrics", {}).get("client", {})
        observations = result.get("observations", {})
        if abs(client.get("final_metronome_level", 0.0) - 0.10) > 0.01:
            return "runtime_metronome_level_not_applied"
        if abs(client.get("final_remote_level", 0.0) - 0.75) > 0.01:
            return "runtime_remote_level_not_applied"
        if not observations.get("server_audio_control_metronome_mode_listener_compensated", False):
            return "runtime_metronome_mode_not_applied"
        return "pass"
    if scenario.startswith("audio-probe-"):
        audio = result.get("audio_probe_analysis", {})
        if not audio:
            return "audio_probe_missing"
        if not audio.get("ok", False):
            return audio.get("verdict", "audio_probe_failed")
        return "pass"
    return "pass"


def metronome_verdict(result, expected_mode):
    metrics = result.get("metrics", {})
    combined = metrics.get("combined", {})
    server = metrics.get("server", {})
    client = metrics.get("client", {})
    if combined.get("metronome_received_min", 0.0) <= 0.0:
        return "metronome_not_observed"
    if combined.get("metronome_alignment_valid_sides", 0.0) < 2:
        return "metronome_alignment_not_valid"
    if combined.get("metronome_epoch_sample_time_min", 0.0) <= 0.0:
        return "metronome_epoch_not_set"
    if combined.get("local_metronome_beat_max", 0.0) <= 0.0 or combined.get("remote_metronome_beat_max", 0.0) <= 0.0:
        return "metronome_beats_not_advancing"
    if expected_mode != "listener-compensated" and combined.get("metronome_beat_delta_abs_max", 0.0) > 2.0:
        return "metronome_grid_beat_delta_high"
    if server.get("metronome_mode", "") != expected_mode or client.get("metronome_mode", "") != expected_mode:
        return "metronome_mode_not_applied"
    if expected_mode == "listener-compensated":
        if combined.get("metronome_compensation_active_sides", 0.0) < 1:
            return "metronome_compensation_not_active"
    else:
        if combined.get("metronome_compensation_offset_ms_abs_max", 0.0) != 0.0:
            return "unexpected_metronome_compensation"
    wav = result.get("metronome_wav_analysis", {})
    if wav:
        if not wav.get("ok", False):
            return wav.get("verdict", "metronome_wav_failed")
    return "pass"


def is_metronome_scenario(scenario):
    source = scenario.get("source_scenario") or scenario.get("scenario") or ""
    return source == "metronome-shared-grid" or source.startswith("metronome-")


def read_wav_i16(path):
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise ValueError("expected mono PCM16 WAV")
        sample_rate = handle.getframerate()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    samples = [
        int.from_bytes(raw[i:i + 2], "little", signed=True)
        for i in range(0, len(raw), 2)
    ]
    return sample_rate, samples


def detect_click_frames(samples):
    threshold = 1800
    holdoff = 900
    clicks = []
    hold = 0
    for index, sample in enumerate(samples):
        if hold > 0:
            hold -= 1
            continue
        if abs(sample) >= threshold:
            clicks.append(index)
            hold = holdoff
    return clicks


def expected_metronome_frames(meta, sample_count):
    sample_rate = int(meta.get("sample_rate", 48000))
    bpm = int(meta.get("bpm", 120))
    division = int(meta.get("metronome_division", 1))
    step_count = max(1, int(meta.get("metronome_step_count", 4)))
    play_low = int(meta.get("metronome_play_mask_low", 0x0f))
    play_high = int(meta.get("metronome_play_mask_high", 0))
    start_audio_frame = int(meta.get("start_audio_frame", 0))
    epoch = int(meta.get("metronome_epoch_sample_time", 0))
    epoch_valid = bool(meta.get("metronome_epoch_valid", False))
    if not epoch_valid or bpm <= 0 or division <= 0:
        return []
    step_interval = max(1, round((60.0 * sample_rate) / (bpm * division)))
    first_absolute = max(start_audio_frame, epoch)
    first_step = max(0, math.floor((first_absolute - epoch) / step_interval) - 1)
    expected = []
    step = first_step
    stop_audio_frame = start_audio_frame + sample_count
    while True:
        absolute = epoch + step * step_interval
        if absolute >= stop_audio_frame:
            break
        if absolute >= start_audio_frame:
            pattern_step = step % step_count
            mask = play_low if pattern_step < 64 else play_high
            bit = pattern_step if pattern_step < 64 else pattern_step - 64
            if ((mask >> bit) & 1) != 0:
                expected.append(absolute - start_audio_frame)
        step += 1
    return expected


def match_clicks(expected, detected):
    errors = []
    missing = 0
    extras = 0
    used = set()
    for frame in expected:
        best_index = None
        best_error = None
        for index, click in enumerate(detected):
            if index in used:
                continue
            error = abs(click - frame)
            if best_error is None or error < best_error:
                best_error = error
                best_index = index
        if best_index is None or best_error is None or best_error > METRONOME_WAV_TOLERANCE_FRAMES:
            missing += 1
        else:
            used.add(best_index)
            errors.append(best_error)
    extras = len(detected) - len(used)
    return {
        "expected_clicks": len(expected),
        "detected_clicks": len(detected),
        "missing_clicks": missing,
        "extra_clicks": extras,
        "max_abs_error_frames": max(errors, default=0),
        "avg_abs_error_frames": sum(errors) / len(errors) if errors else 0.0,
    }


def classify_metronome_clicks(expected, detected, analysis):
    startup_boundary = False
    steady_missing = analysis["missing_clicks"]
    steady_extra = analysis["extra_clicks"]
    steady_max_error = analysis["max_abs_error_frames"]
    if analysis["missing_clicks"] or analysis["extra_clicks"]:
        for detected_start in range(1, min(3, len(detected)) + 1):
            if len(expected) <= 1 or len(expected[1:]) != len(detected[detected_start:]):
                continue
            steady_errors = [abs(click - frame) for frame, click in zip(expected[1:], detected[detected_start:])]
            if not steady_errors or any(error > METRONOME_WAV_TOLERANCE_FRAMES for error in steady_errors):
                continue
            startup_boundary = True
            steady_missing = 0
            steady_extra = 0
            steady_max_error = max(steady_errors, default=0)
            break
    analysis.update({
        "startup_boundary_mismatch": startup_boundary,
        "steady_missing_clicks": steady_missing,
        "steady_extra_clicks": steady_extra,
        "steady_max_abs_error_frames": steady_max_error,
    })
    return analysis


def analyze_side_recording(recording_dir, allow_silent=False):
    meta_path = Path(recording_dir) / "recording.json"
    wav_path = Path(recording_dir) / "metronome.wav"
    if not meta_path.exists() or not wav_path.exists():
        return {"ok": False, "verdict": "metronome_wav_missing", "recording_dir": str(recording_dir)}
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    sample_rate, samples = read_wav_i16(wav_path)
    detected = detect_click_frames(samples)
    expected = expected_metronome_frames(meta, len(samples))
    if allow_silent and not detected:
        return {
            "ok": True,
            "recording_dir": str(recording_dir),
            "sample_rate": sample_rate,
            "expected_clicks": len(expected),
            "detected_clicks": 0,
            "silent_allowed": True,
        }
    analysis = classify_metronome_clicks(expected, detected, match_clicks(expected, detected))
    analysis.update({
        "ok": (
            analysis["steady_missing_clicks"] == 0 and
            analysis["steady_extra_clicks"] == 0 and
            analysis["steady_max_abs_error_frames"] <= METRONOME_WAV_TOLERANCE_FRAMES),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
    })
    if not analysis["ok"]:
        analysis["verdict"] = (
            "metronome_click_count_mismatch"
            if analysis["steady_missing_clicks"] > 0 or analysis["steady_extra_clicks"] > 0
            else "metronome_click_timing_high"
        )
    elif analysis["startup_boundary_mismatch"]:
        analysis["verdict"] = "pass_startup_boundary"
    return analysis


def analyze_metronome_recordings(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if not (scenario == "metronome-shared-grid" or scenario.startswith("metronome-")):
        return {}
    allow_client_silent = scenario == "metronome-leader-audio"
    loose_timing = scenario.startswith("metronome-listener-compensated")
    server = analyze_side_recording(Path(server_paths["dir"]) / "recording")
    client = analyze_side_recording(Path(client_paths["dir"]) / "recording", allow_silent=allow_client_silent)
    if loose_timing:
        def clean_loose(side, missing_verdict):
            detected = side.get("detected_clicks", 0)
            return {
                "ok": detected > 0,
                "verdict": "pass_loose_listener_compensated" if detected > 0 else missing_verdict,
                "recording_dir": side.get("recording_dir", ""),
                "sample_rate": side.get("sample_rate", 0),
                "detected_clicks": detected,
                "strict_grid_expected_clicks": side.get("expected_clicks", 0),
                "strict_grid_check": "skipped_listener_compensated",
            }
        server = clean_loose(server, "listener_compensated_server_clicks_missing")
        client = clean_loose(client, "listener_compensated_client_clicks_missing")
    ok = server.get("ok", False) and client.get("ok", False)
    verdict = ""
    if not ok:
        verdict = server.get("verdict") if not server.get("ok", False) else client.get("verdict", "metronome_wav_failed")
    return {"ok": ok, "verdict": verdict, "server": server, "client": client}


def is_listener_compensated_pulse_tracking_scenario(scenario):
    return (
        scenario == "metronome-listener-compensated-metro-pulse" or
        scenario.startswith("metronome-listener-compensated-pulse"))


def nearest_errors(reference_frames, measured_frames):
    errors = []
    used = set()
    missing = 0
    for reference in reference_frames:
        best_index = None
        best_abs = None
        best_signed = None
        for index, measured in enumerate(measured_frames):
            if index in used:
                continue
            signed = measured - reference
            absolute = abs(signed)
            if best_abs is None or absolute < best_abs:
                best_index = index
                best_abs = absolute
                best_signed = signed
        if best_index is None:
            missing += 1
            continue
        used.add(best_index)
        errors.append(best_signed)
    return errors, missing, max(0, len(measured_frames) - len(used))


def summarize_signed_errors(errors, sample_rate):
    if not errors:
        return {
            "samples": 0,
            "avg_error_frames": 0.0,
            "avg_error_ms": 0.0,
            "avg_abs_error_ms": 0.0,
            "max_abs_error_ms": 0.0,
            "min_error_ms": 0.0,
            "max_error_ms": 0.0,
        }
    ms = [error * 1000.0 / sample_rate for error in errors]
    abs_ms = [abs(value) for value in ms]
    return {
        "samples": len(errors),
        "avg_error_frames": sum(errors) / len(errors),
        "avg_error_ms": sum(ms) / len(ms),
        "avg_abs_error_ms": sum(abs_ms) / len(abs_ms),
        "max_abs_error_ms": max(abs_ms),
        "min_error_ms": min(ms),
        "max_error_ms": max(ms),
    }


def read_stem_frames(recording_dir, stem, threshold, refractory_frames):
    wav_path = Path(recording_dir) / f"{stem}.wav"
    if not wav_path.exists():
        return 0, []
    sample_rate, samples = read_wav_i16(wav_path)
    return sample_rate, detect_click_frames_with_threshold(samples, threshold, refractory_frames)


def detect_click_frames_with_threshold(samples, threshold, refractory_frames):
    frames = []
    holdoff = 0
    for index, sample in enumerate(samples):
        if holdoff > 0:
            holdoff -= 1
            continue
        if abs(sample) >= threshold:
            frames.append(index)
            holdoff = refractory_frames
    return frames


def analyze_listener_compensated_pulse_side(recording_dir):
    recording_dir = Path(recording_dir)
    sample_rate, pulses = read_stem_frames(recording_dir, "their-input", 2500, 18000)
    metro_rate, clicks = read_stem_frames(recording_dir, "metronome", 1800, 900)
    if sample_rate == 0:
        sample_rate = metro_rate
    if sample_rate == 0:
        return {
            "ok": False,
            "verdict": "listener_compensated_pulse_recording_missing",
            "recording_dir": str(recording_dir),
            "sample_rate": 0,
            "remote_pulses_detected": len(pulses),
            "metronome_clicks_detected": len(clicks),
            "matched_pulses": 0,
            "missing_pulse_matches": 0,
            "extra_clicks": 0,
        }
    errors, missing_pulses, extra_clicks = nearest_errors(pulses, clicks)
    summary = summarize_signed_errors(errors, sample_rate)
    summary.update({
        "ok": bool(pulses) and bool(clicks),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
        "remote_pulses_detected": len(pulses),
        "metronome_clicks_detected": len(clicks),
        "matched_pulses": len(errors),
        "missing_pulse_matches": missing_pulses,
        "extra_clicks": extra_clicks,
        "verdict": "pass" if pulses and clicks else "pulse_or_click_missing",
    })
    return summary


def analyze_metro_pulse_epoch_side(recording_dir):
    recording_dir = Path(recording_dir)
    meta_path = recording_dir / "recording.json"
    wav_path = recording_dir / "my-input.wav"
    if not meta_path.exists() or not wav_path.exists():
        return {
            "ok": False,
            "verdict": "metro_pulse_epoch_recording_missing",
            "recording_dir": str(recording_dir),
        }
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    sample_rate, samples = read_wav_i16(wav_path)
    expected = expected_metronome_frames(meta, len(samples))
    detected = detect_click_frames_with_threshold(samples, 2500, 900)
    analysis = classify_metronome_clicks(expected, detected, match_clicks(expected, detected))
    analysis.update({
        "ok": (
            analysis["steady_missing_clicks"] == 0 and
            analysis["steady_extra_clicks"] == 0 and
            analysis["steady_max_abs_error_frames"] <= METRONOME_WAV_TOLERANCE_FRAMES),
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
    })
    if not analysis["ok"]:
        analysis["verdict"] = (
            "metro_pulse_epoch_count_mismatch"
            if analysis["steady_missing_clicks"] > 0 or analysis["steady_extra_clicks"] > 0
            else "metro_pulse_epoch_timing_high"
        )
    elif analysis["startup_boundary_mismatch"]:
        analysis["verdict"] = "pass_startup_boundary"
    else:
        analysis["verdict"] = "pass"
    return analysis


def analyze_metro_pulse_epoch(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if scenario != "metronome-listener-compensated-metro-pulse":
        return {}
    server = analyze_metro_pulse_epoch_side(Path(server_paths["dir"]) / "recording")
    client = analyze_metro_pulse_epoch_side(Path(client_paths["dir"]) / "recording")
    ok = server.get("ok", False) and client.get("ok", False)
    return {
        "ok": ok,
        "verdict": "pass" if ok else "metro_pulse_epoch_analysis_failed",
        "server": server,
        "client": client,
        "combined": {
            "max_abs_error_frames_max": max(
                server.get("steady_max_abs_error_frames", 0),
                client.get("steady_max_abs_error_frames", 0)),
            "missing_clicks_total": server.get("steady_missing_clicks", 0) + client.get("steady_missing_clicks", 0),
            "extra_clicks_total": server.get("steady_extra_clicks", 0) + client.get("steady_extra_clicks", 0),
        },
    }


def analyze_listener_compensated_pulse(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if not is_listener_compensated_pulse_tracking_scenario(scenario):
        return {}
    server = analyze_listener_compensated_pulse_side(Path(server_paths["dir"]) / "recording")
    client = analyze_listener_compensated_pulse_side(Path(client_paths["dir"]) / "recording")
    ok = server.get("ok", False) and client.get("ok", False)
    return {
        "ok": ok,
        "verdict": "pass" if ok else "listener_compensated_pulse_analysis_failed",
        "server": server,
        "client": client,
        "combined": {
            "matched_pulses_min": min(server.get("matched_pulses", 0), client.get("matched_pulses", 0)),
            "avg_abs_error_ms_max": max(server.get("avg_abs_error_ms", 0.0), client.get("avg_abs_error_ms", 0.0)),
            "max_abs_error_ms_max": max(server.get("max_abs_error_ms", 0.0), client.get("max_abs_error_ms", 0.0)),
            "missing_pulse_matches_total": server.get("missing_pulse_matches", 0) + client.get("missing_pulse_matches", 0),
        },
    }


def run_runtime_commands(commands, server_process, client_process):
    start = time.monotonic()
    for command in sorted(commands, key=lambda item: item.get("at_s", 0.0)):
        delay = start + float(command.get("at_s", 0.0)) - time.monotonic()
        if delay > 0.0:
            time.sleep(delay)
        target = server_process if command.get("side") == "server" else client_process
        target.send_line(command.get("line", ""))


def text_contains(path, pattern):
    try:
        return pattern in Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def scenario_observations(scenario_id, scenario, server_paths, client_paths):
    source = scenario.get("source_scenario", scenario_id)
    observations = {}
    if source == "runtime-controls":
        observations["server_audio_control_metronome_mode_listener_compensated"] = text_contains(
            server_paths["stdout"],
            "Audio control metronome mode: listener-compensated")
        observations["client_final_metronome_level_0_1"] = text_contains(
            client_paths["stdout"],
            "Final metronome level: 0.1")
        observations["client_final_remote_level_0_75"] = text_contains(
            client_paths["stdout"],
            "Final remote playback level: 0.75")
    return observations


def run_scenario(jam2, scenario_id, scenario, args_ns, output_dir, seed):
    profile = scenario["profile"]
    os_priority = scenario.get("os_priority", "high")
    source_scenario = scenario.get("source_scenario", scenario_id)
    record_metronome = source_scenario == "metronome-shared-grid" or source_scenario.startswith("metronome-")
    audio_probe = source_scenario.startswith("audio-probe-")
    commands = list(scenario.get("commands", []))
    server_extra_args = ["--os-priority", os_priority]
    client_extra_args = ["--os-priority", os_priority]
    server_signal = scenario.get("server_signal", scenario.get("signal", "silence"))
    client_signal = scenario.get("client_signal", scenario.get("signal", "silence"))
    if audio_probe:
        server_recording = Path(output_dir) / "server" / "recording"
        client_recording = Path(output_dir) / "client" / "recording"
        server_extra_args.extend([
            "--record-jam-folder", str(server_recording),
            "--test-input", server_signal,
        ])
        client_extra_args.extend([
            "--record-jam-folder", str(client_recording),
            "--test-input", client_signal,
        ])
    elif scenario.get("server_signal") or scenario.get("client_signal"):
        server_extra_args.extend(["--test-input", server_signal])
        client_extra_args.extend(["--test-input", client_signal])
    if record_metronome:
        commands.extend([
            {"at_s": 1.0, "side": "server", "line": f"record jam start {Path(output_dir) / 'server' / 'recording'}"},
            {"at_s": 1.0, "side": "client", "line": f"record jam start {Path(output_dir) / 'client' / 'recording'}"},
            {"at_s": max(2.0, args_ns.stream_ms / 1000.0 - 1.0), "side": "server", "line": "record jam stop"},
            {"at_s": max(2.0, args_ns.stream_ms / 1000.0 - 1.0), "side": "client", "line": "record jam stop"},
        ])
    print_flush(f"[stress] starting {scenario_id} profile={profile.name} os_priority={os_priority}")
    listener, server_paths = start_listener(
        jam2,
        args_ns.server_audio_device,
        args_ns.sample_rate,
        profile,
        args_ns.stream_ms,
        output_dir,
        extra_args=server_extra_args,
        stdin_pipe=bool(commands))
    startup = listener.wait_for_startup("waiting", args_ns.startup_timeout_s)
    if startup is None:
        listener.terminate()
        return {
            "scenario": scenario_id,
            "profile": profile.name,
            "error": "listener did not emit waiting startup JSON",
            "server_return_code": listener.poll(),
            "client_return_code": None,
        }

    real_endpoint = parse_endpoint(startup["local_endpoint"])
    proxy = UdpStressProxy(real_endpoint, impairment=scenario["impairment"], seed=seed)
    stop_proxy = threading.Event()
    proxy_thread = threading.Thread(target=proxy.run_until, args=(stop_proxy,), daemon=True)
    proxy_thread.start()

    proxy_url = rewrite_jam_url_endpoint(startup["connection_url"], proxy.public_endpoint)
    connector, client_paths = start_connector(
        jam2,
        proxy_url,
        args_ns.client_audio_device,
        args_ns.sample_rate,
        profile,
        args_ns.stream_ms,
        output_dir,
        extra_args=client_extra_args,
        stdin_pipe=bool(commands))

    command_thread = None
    if commands:
        command_thread = threading.Thread(
            target=run_runtime_commands,
            args=(commands, listener, connector),
            daemon=True)
        command_thread.start()

    client_rc = connector.wait(timeout=max(30.0, args_ns.stream_ms / 1000.0 + 30.0))
    if client_rc is None:
        connector.terminate()
        client_rc = connector.poll()
    server_rc = listener.wait(timeout=10.0)
    if server_rc is None:
        listener.terminate()
        server_rc = listener.poll()
    if command_thread is not None:
        command_thread.join(timeout=1.0)

    stop_proxy.set()
    proxy_thread.join(timeout=2.0)
    proxy_endpoint = proxy.public_endpoint
    proxy_stats = dict(proxy.stats)
    proxy.close()

    server_csv = collect_side_csv(server_paths)
    client_csv = collect_side_csv(client_paths)
    metrics = combined_summary(server_csv, client_csv)
    observations = scenario_observations(scenario_id, scenario, server_paths, client_paths)
    result = {
        "scenario": scenario_id,
        "source_scenario": scenario.get("source_scenario", scenario_id),
        "profile_family": scenario.get("profile_family", args_ns.profile),
        "profile": profile.name,
        "os_priority": os_priority,
        "expect": scenario["expect"],
        "profile_metadata": profile.metadata(),
        "server_return_code": server_rc,
        "client_return_code": client_rc,
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv else "",
        "proxy_endpoint": f"{proxy_endpoint[0]}:{proxy_endpoint[1]}",
        "real_server_endpoint": startup["local_endpoint"],
        "proxy_stats": proxy_stats,
        "observations": observations,
        "metrics": metrics,
    }
    wav_analysis = analyze_metronome_recordings(result, server_paths, client_paths)
    if wav_analysis:
        result["metronome_wav_analysis"] = wav_analysis
    metro_pulse_epoch = analyze_metro_pulse_epoch(result, server_paths, client_paths)
    if metro_pulse_epoch:
        result["metro_pulse_epoch_analysis"] = metro_pulse_epoch
    pulse_analysis = analyze_listener_compensated_pulse(result, server_paths, client_paths)
    if pulse_analysis:
        result["listener_compensated_pulse_analysis"] = pulse_analysis
    if audio_probe:
        server_analysis = analyze_recording_dir(
            Path(server_paths["dir"]) / "recording",
            scenario.get("signal", "silence"),
            local_signal=scenario.get("server_signal", "silence"),
            remote_signal=scenario.get("client_signal", "silence"))
        client_analysis = analyze_recording_dir(
            Path(client_paths["dir"]) / "recording",
            scenario.get("signal", "silence"),
            local_signal=scenario.get("client_signal", "silence"),
            remote_signal=scenario.get("server_signal", "silence"))
        server_ok = audio_probe_health_ok(server_analysis)
        client_ok = audio_probe_health_ok(client_analysis)
        result["audio_probe_analysis"] = {
            "ok": server_ok and client_ok,
            "verdict": "pass" if server_ok and client_ok else "audio_probe_analysis_failed",
            "signal": scenario.get("signal", ""),
            "server_signal": scenario.get("server_signal", ""),
            "client_signal": scenario.get("client_signal", ""),
            "server": server_analysis,
            "client": client_analysis,
            "tags": sorted(set(server_analysis.get("tags", []) + client_analysis.get("tags", []))),
        }
    result["verdict"] = verdict_for(result)
    print_flush(
        f"[stress] finished {scenario_id} verdict={result['verdict']} "
        f"loss_max={metrics['combined'].get('loss_percent_max', 0.0):.3f}% "
        f"jitter_max={metrics['combined'].get('jitter_max_ms', 0.0):.1f}ms "
        f"underrun_ms={metrics['combined'].get('playback_underrun_time_ms_total', 0.0):.1f}")
    return result


def write_summary(path, results):
    lines = []
    for result in results:
        if "mesh_metrics" in result:
            metrics = result.get("mesh_metrics", {})
            lines.append(
                f"{result.get('scenario')} verdict={result.get('verdict')} "
                f"mesh_peers={metrics.get('peer_count', 0)} "
                f"recv_min={metrics.get('recv_packets_min', 0.0):.0f} "
                f"sent_min={metrics.get('sent_packets_min', 0.0):.0f} "
                f"loss_total={metrics.get('sequence_lost_total', 0.0):.0f} "
                f"jitter_max={metrics.get('jitter_max_ms', 0.0):.1f}ms "
                f"rtt_max={metrics.get('rtt_max_ms', 0.0):.1f}ms "
                f"underrun_ms={metrics.get('playback_underrun_time_ms_total', 0.0):.1f} "
                f"audio_ok={metrics.get('audio_ok_peers', 0)}/{metrics.get('peer_count', 0)} "
                f"audio_tags={','.join(metrics.get('audio_tags', [])) or '-'}")
            continue
        metrics = result.get("metrics", {}).get("combined", {})
        audio_probe = result.get("audio_probe_analysis", {})
        audio_suffix = ""
        if audio_probe:
            audio_suffix = (
                f" audio_probe_ok={'yes' if audio_probe.get('ok', False) else 'no'}"
                f" audio_tags={','.join(audio_probe.get('tags', [])) or '-'}")
        pulse = result.get("listener_compensated_pulse_analysis", {})
        pulse_suffix = ""
        if pulse:
            combined = pulse.get("combined", {})
            pulse_suffix = (
                f" listener_pulse_ok={'yes' if pulse.get('ok', False) else 'no'}"
                f" listener_pulse_matched_min={combined.get('matched_pulses_min', 0)}"
                f" listener_pulse_avg_abs_ms={combined.get('avg_abs_error_ms_max', 0.0):.2f}"
                f" listener_pulse_max_abs_ms={combined.get('max_abs_error_ms_max', 0.0):.2f}")
        metro_pulse = result.get("metro_pulse_epoch_analysis", {})
        if metro_pulse:
            combined = metro_pulse.get("combined", {})
            pulse_suffix += (
                f" metro_pulse_epoch_ok={'yes' if metro_pulse.get('ok', False) else 'no'}"
                f" metro_pulse_epoch_max_error_frames={combined.get('max_abs_error_frames_max', 0)}")
        lines.append(
            f"{result.get('scenario')} verdict={result.get('verdict')} "
            f"loss_max={metrics.get('loss_percent_max', 0.0):.3f}% "
            f"jitter_max={metrics.get('jitter_max_ms', 0.0):.1f}ms "
            f"rtt_max={metrics.get('rtt_max_ms', 0.0):.1f}ms "
            f"underrun_ms={metrics.get('playback_underrun_time_ms_total', 0.0):.1f} "
            f"drift_abs_ppm={metrics.get('drift_abs_ppm_max', 0.0):.1f}"
            f"{audio_suffix}"
            f"{pulse_suffix}"
        )
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def validation_command(jam2, args, output_dir, expect_rc, name):
    output_dir = ensure_dir(output_dir)
    process = ManagedProcess(
        [jam2] + args,
        Path.cwd(),
        output_dir / "stdout.txt",
        output_dir / "stderr.txt").start()
    rc = process.wait(timeout=20.0)
    if rc is None:
        process.terminate()
        rc = process.poll()
    return {
        "validation": name,
        "return_code": rc,
        "expected_return_code": expect_rc,
        "verdict": "pass" if rc == expect_rc else "unexpected_return_code",
        "args": [str(item) for item in args],
    }


def profile_explicit_args(profile, stream_ms):
    args = profile.args(stream_ms)
    cleaned = []
    index = 0
    while index < len(args):
        if args[index] == "--profile":
            index += 2
            continue
        cleaned.append(args[index])
        index += 1
    return cleaned


def read_startup_json(stdout_path):
    for line in Path(stdout_path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("{"):
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if payload.get("event") == "startup":
                return payload
    return None


def run_startup_validation_command(jam2, args, output_dir):
    output_dir = ensure_dir(output_dir)
    process = ManagedProcess(
        [jam2] + args,
        Path.cwd(),
        output_dir / "stdout.txt",
        output_dir / "stderr.txt").start()
    rc = process.wait(timeout=20.0)
    if rc is None:
        process.terminate()
        rc = process.poll()
    return rc, read_startup_json(output_dir / "stdout.txt")


def validation_profile_equivalence(jam2, profile_name, profile, output_dir, stream_ms):
    output_dir = ensure_dir(output_dir)
    common = [
        "listen",
        "--bind", "127.0.0.1:0",
        "--no-stun",
        "--wait-ms", "250",
        "--machine-readable-startup", "on",
    ]
    profile_args = common + ["--profile", profile_name]
    explicit_args = common + [
        "--sample-rate", str(profile.sample_rate),
    ] + profile_explicit_args(profile, stream_ms)
    profile_rc, profile_startup = run_startup_validation_command(
        jam2,
        profile_args,
        output_dir / "profile")
    explicit_rc, explicit_startup = run_startup_validation_command(
        jam2,
        explicit_args,
        output_dir / "explicit")
    fields = [
        "sample_rate",
        "frame_size",
        "audio_buffer_size",
        "capture_ring_frames",
        "playback_ring_frames",
        "playback_prefill_frames",
        "playback_max_frames",
        "drift_correction",
        "drift_smoothing",
        "drift_deadband_ppm",
        "drift_max_correction_ppm",
        "sample_time_playout",
        "playout_delay_frames",
        "jitter_buffer_frames",
        "jitter_buffer_max_frames",
        "adaptive_playback_cushion",
        "adaptive_playback_target_frames",
        "adaptive_playback_min_frames",
        "adaptive_playback_max_frames",
        "adaptive_playback_release_ppm",
    ]
    mismatches = []
    if profile_startup is None or explicit_startup is None:
        mismatches.append("missing startup JSON")
    else:
        for field in fields:
            if profile_startup.get(field) != explicit_startup.get(field):
                mismatches.append(
                    f"{field}: profile={profile_startup.get(field)!r} explicit={explicit_startup.get(field)!r}")
    ok = profile_rc == 3 and explicit_rc == 3 and not mismatches
    return {
        "validation": f"profile-equivalence-{profile_name}",
        "verdict": "pass" if ok else "profile_mismatch",
        "profile_return_code": profile_rc,
        "explicit_return_code": explicit_rc,
        "expected_return_code": 3,
        "mismatches": mismatches,
        "profile_args": [str(item) for item in profile_args],
        "explicit_args": [str(item) for item in explicit_args],
        "profile_startup": profile_startup or {},
        "explicit_startup": explicit_startup or {},
    }


def validation_profile_mismatch(jam2, name, args_ns, output_dir, server_profile, client_profile, client_extra_args=None):
    return run_pair_validation(
        jam2,
        name,
        args_ns,
        output_dir,
        server_profile,
        client_profile,
        False,
        None,
        client_extra_args=client_extra_args)


def replace_url_key(url, key_hex):
    parts = []
    for item in url.split("&"):
        if item.startswith("key="):
            parts.append("key=" + key_hex)
        else:
            parts.append(item)
    return "&".join(parts)


def run_pair_validation(
        jam2,
        name,
        args_ns,
        output_dir,
        server_profile,
        client_profile,
        expect_success,
        url_mutator=None,
        server_extra_args=None,
        client_extra_args=None):
    listener, server_paths = start_listener(
        jam2,
        args_ns.server_audio_device,
        args_ns.sample_rate,
        server_profile,
        args_ns.validation_stream_ms,
        output_dir,
        extra_args=server_extra_args)
    startup = listener.wait_for_startup("waiting", args_ns.startup_timeout_s)
    if startup is None:
        listener.terminate()
        return {
            "validation": name,
            "verdict": "listener_startup_failed",
            "server_return_code": listener.poll(),
            "client_return_code": None,
        }

    real_endpoint = parse_endpoint(startup["local_endpoint"])
    proxy = UdpStressProxy(real_endpoint, impairment=ProxyImpairment.both(DirectionImpairment()), seed=args_ns.seed)
    stop_proxy = threading.Event()
    proxy_thread = threading.Thread(target=proxy.run_until, args=(stop_proxy,), daemon=True)
    proxy_thread.start()
    url = rewrite_jam_url_endpoint(startup["connection_url"], proxy.public_endpoint)
    if url_mutator is not None:
        url = url_mutator(url)

    connector, client_paths = start_connector(
        jam2,
        url,
        args_ns.client_audio_device,
        args_ns.sample_rate,
        client_profile,
        args_ns.validation_stream_ms,
        output_dir,
        extra_args=client_extra_args)
    client_rc = connector.wait(timeout=max(25.0, args_ns.validation_stream_ms / 1000.0 + 20.0))
    if client_rc is None:
        connector.terminate()
        client_rc = connector.poll()
    server_rc = listener.wait(timeout=10.0)
    if server_rc is None:
        listener.terminate()
        server_rc = listener.poll()

    stop_proxy.set()
    proxy_thread.join(timeout=2.0)
    proxy.close()
    server_csv = collect_side_csv(server_paths)
    client_csv = collect_side_csv(client_paths)
    ok = (server_rc == 0 and client_rc == 0) if expect_success else (server_rc != 0 or client_rc != 0)
    return {
        "validation": name,
        "verdict": "pass" if ok else "unexpected_success" if not expect_success else "unexpected_failure",
        "server_return_code": server_rc,
        "client_return_code": client_rc,
        "server_args": listener.args,
        "client_args": connector.args,
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv else "",
    }


def run_validations(jam2, args_ns, logs):
    validations_dir = ensure_dir(logs / "validation")
    results = []
    results.append(validation_command(
        jam2,
        ["listen", "--profile", "fast", "--definitely-bad-option"],
        validations_dir / "bad-option",
        1,
        "bad-option"))
    results.append(validation_command(
        jam2,
        ["connect", "not-a-jam-url", "--profile", "fast", "--wait-ms", "100"],
        validations_dir / "bad-url",
        1,
        "bad-url"))
    results.append(validation_command(
        jam2,
        ["listen", "--profile", "fast", "--bind", "127.0.0.1:0", "--no-stun", "--wait-ms", "250", "--machine-readable-startup", "on"],
        validations_dir / "listen-timeout",
        3,
        "listen-timeout"))
    for profile_name, profile in (
            ("fast", FAST_PROFILE),
            ("moderate", MODERATE_PROFILE),
            ("safe", SAFE_PROFILE)):
        results.append(validation_profile_equivalence(
            jam2,
            profile_name,
            profile,
            validations_dir / f"profile-equivalence-{profile_name}",
            args_ns.validation_stream_ms))
    results.append(validation_profile_mismatch(
        jam2,
        "profile-mismatch-fast-safe",
        args_ns,
        validations_dir / "profile-mismatch-fast-safe",
        FAST_PROFILE,
        SAFE_PROFILE))
    results.append(validation_profile_mismatch(
        jam2,
        "profile-override-frame-size-mismatch",
        args_ns,
        validations_dir / "profile-override-frame-size-mismatch",
        FAST_PROFILE,
        FAST_PROFILE,
        client_extra_args=["--frame-size", "128"]))
    fixed_session_profile = MODERATE_PROFILE
    session_args = ["--session-id", "1", "--session-key", "00000000000000000000000000000001"]
    results.append(run_pair_validation(
        jam2,
        "session-override",
        args_ns,
        validations_dir / "session-override",
        fixed_session_profile,
        fixed_session_profile,
        True,
        None,
        server_extra_args=session_args))
    results.append(run_pair_validation(
        jam2,
        "wrong-key-timeout",
        args_ns,
        validations_dir / "wrong-key-timeout",
        MODERATE_PROFILE,
        MODERATE_PROFILE,
        False,
        lambda url: replace_url_key(url, "ffffffffffffffffffffffffffffffff")))
    results.append(run_pair_validation(
        jam2,
        "mismatched-frame-size",
        args_ns,
        validations_dir / "mismatched-frame-size",
        MODERATE_PROFILE,
        variant(MODERATE_PROFILE, "frame_256", frame_size=256),
        False,
        None))
    return results


def main():
    args_ns = parse_args()
    jam2 = Path(args_ns.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    if args_ns.stream_ms <= 0:
        return fail("--stream-ms must be positive")
    if args_ns.mode == "normal" and (not args_ns.server_audio_device or not args_ns.client_audio_device):
        return fail("--server-audio-device and --client-audio-device are required in --mode normal")

    logs = Path(args_ns.logs)
    if args_ns.clean and logs.exists():
        shutil.rmtree(logs)
    ensure_dir(logs)

    if args_ns.mode == "mesh":
        try:
            counts = mesh_peer_counts(args_ns)
            catalog, scenario_ids = mesh_scenario_plan(args_ns.profile, args_ns.scenario, counts)
        except ValueError as error:
            return fail(str(error))
    else:
        catalog, scenario_ids = scenario_plan(args_ns.profile, args_ns.scenario, args_ns.include_audio_probes)
    os_priorities = selected_os_priorities(args_ns.os_priority)
    unknown = [scenario for scenario in scenario_ids if scenario not in catalog]
    if unknown:
        return fail("unknown scenario(s): " + ", ".join(unknown))

    results = []
    run_plan = []
    for scenario_id in scenario_ids:
        for os_priority in os_priorities:
            planned_id = scenario_id if len(os_priorities) == 1 else f"{scenario_id}__os_{os_priority}"
            run_plan.append((planned_id, scenario_id, os_priority))

    for index, (planned_id, scenario_id, os_priority) in enumerate(run_plan):
        scenario_dir = ensure_dir(logs / f"{index + 1:02d}_{safe_test_id(planned_id)}")
        scenario = dict(catalog[scenario_id])
        scenario.setdefault("source_scenario", scenario_id)
        scenario["os_priority"] = os_priority
        write_json(scenario_dir / "scenario.json", {
            "scenario": planned_id,
            "source_scenario": scenario.get("source_scenario", scenario_id),
            "profile_family": scenario.get("profile_family", args_ns.profile),
            "mode": args_ns.mode,
            "stream_ms": args_ns.stream_ms,
            "sample_rate": args_ns.sample_rate,
            "server_audio_device": args_ns.server_audio_device,
            "client_audio_device": args_ns.client_audio_device,
            "mesh_peers": scenario.get("mesh_peers", 0),
            "profile": scenario["profile"].metadata(),
            "os_priority": os_priority,
            "expect": scenario["expect"],
            "signal": scenario.get("signal", ""),
            "server_signal": scenario.get("server_signal", ""),
            "client_signal": scenario.get("client_signal", ""),
        })
        if args_ns.mode == "mesh":
            result = run_mesh_scenario(jam2, planned_id, scenario, args_ns, scenario_dir, args_ns.seed + index)
        else:
            result = run_scenario(jam2, planned_id, scenario, args_ns, scenario_dir, args_ns.seed + index)
        write_json(scenario_dir / "result.json", result)
        results.append(result)
        time.sleep(0.5)

    write_json(logs / "stress_results.json", {"results": results})
    write_results_csv(logs / "stress_results.csv", results)
    write_summary(logs / "stress_summary.txt", results)
    validation_results = []
    if args_ns.include_validation:
        validation_results = run_validations(jam2, args_ns, logs)
        write_json(logs / "validation_results.json", {"results": validation_results})
    print_flush(f"[stress] wrote {logs / 'stress_results.json'}")
    print_flush(f"[stress] wrote {logs / 'stress_results.csv'}")
    print_flush(f"[stress] wrote {logs / 'stress_summary.txt'}")
    if args_ns.include_validation:
        print_flush(f"[stress] wrote {logs / 'validation_results.json'}")
    failures = [result for result in results if result.get("verdict") != "pass"]
    failures.extend(result for result in validation_results if result.get("verdict") != "pass")
    return 2 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
