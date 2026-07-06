#!/usr/bin/env python3

import argparse
import json
import math
import shutil
import wave
import threading
import time
from pathlib import Path

from jam2_harness import (
    collect_side_csv,
    ManagedProcess,
    parse_endpoint,
    rewrite_jam_url_endpoint,
    start_connector,
    start_listener,
)
from jam2_metrics import combined_summary, write_results_csv
from jam2_profiles import AGGRESSIVE_LOCAL_PROFILE, SAFE_LOCAL_PROFILE, adaptive_off_profile, variant
from matrix_common import default_jam2_path, ensure_dir, fail, print_flush, safe_test_id, write_json
from udp_stress_proxy import DirectionImpairment, ProxyImpairment, UdpStressProxy


DEFAULT_STREAM_MS = 30000
METRONOME_WAV_TOLERANCE_FRAMES = 96


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run localhost Jam2 stress scenarios through a UDP impairment proxy using two local audio devices.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--server-audio-device", required=True)
    parser.add_argument("--client-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("stress_logs")))
    parser.add_argument("--stream-ms", type=int, default=DEFAULT_STREAM_MS)
    parser.add_argument("--startup-timeout-s", type=float, default=10.0)
    parser.add_argument("--clean", action="store_true", help="delete the stress log directory before running")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument(
        "--profile",
        choices=("aggressive", "safe", "both"),
        default="aggressive",
        help="profile family for stress scenarios; both duplicates selected scenarios for comparison")
    parser.add_argument("--include-validation", action="store_true", help="also run short CLI/session/error validation checks")
    parser.add_argument("--validation-stream-ms", type=int, default=5000)
    parser.add_argument(
        "--scenario",
        action="append",
        default=[],
        help="scenario id to run; repeat for multiple. Default runs the standard suite.")
    return parser.parse_args()


def selected_profile(profile_name):
    if profile_name == "safe":
        return SAFE_LOCAL_PROFILE
    if profile_name == "aggressive":
        return AGGRESSIVE_LOCAL_PROFILE
    raise ValueError(f"unknown profile: {profile_name}")


def scenario_catalog(base_profile=AGGRESSIVE_LOCAL_PROFILE):
    return {
        "clean-control": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "no injected impairment",
        },
        "clean-safe-control": {
            "profile": SAFE_LOCAL_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "safe localhost baseline with no injected impairment",
        },
        "clean-aggressive-control": {
            "profile": AGGRESSIVE_LOCAL_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "aggressive localhost baseline with no injected impairment",
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
        "metronome-symmetric-delay": {
            "profile": variant(base_profile, "metro_symmetric_delay", metronome_mode="symmetric-delay"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "symmetric-delay metronome mode should run and exchange metronome state",
        },
        "metronome-listener-compensated": {
            "profile": variant(base_profile, "metro_listener_compensated", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "listener-compensated metronome mode should run and exchange metronome state",
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
                {"at_s": 7.0, "side": "server", "line": "metro mode symmetric-delay"},
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
        "metronome-shared-grid",
        "metronome-leader-audio",
        "metronome-symmetric-delay",
        "metronome-listener-compensated",
        "levels-low",
        "sample-time-playout-off",
        "playout-delay-3072",
        "drift-max-5ppm",
        "drift-smoothing-fast",
        "socket-buffers",
        "channels-1-to-1",
        "runtime-controls",
    ]


def expand_scenarios(scenario_ids):
    expanded = []
    for scenario_id in scenario_ids:
        if scenario_id == "adaptive-off-vs-on":
            expanded.extend(["adaptive-on-pressure", "adaptive-off-pressure"])
        else:
            expanded.append(scenario_id)
    return expanded


def scenario_plan(profile_mode, requested_scenarios):
    base_ids = expand_scenarios(requested_scenarios) if requested_scenarios else standard_suite()
    if profile_mode != "both":
        return scenario_catalog(selected_profile(profile_mode)), base_ids

    planned_catalog = {}
    planned_ids = []
    for profile_name in ("aggressive", "safe"):
        catalog = scenario_catalog(selected_profile(profile_name))
        for scenario_id in base_ids:
            planned_id = f"{profile_name}__{scenario_id}"
            scenario = dict(catalog[scenario_id])
            scenario["source_scenario"] = scenario_id
            scenario["profile_family"] = profile_name
            planned_catalog[planned_id] = scenario
            planned_ids.append(planned_id)
    return planned_catalog, planned_ids


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
    if scenario == "metronome-shared-grid":
        return metronome_verdict(result, "shared-grid")
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
        if not observations.get("server_audio_control_metronome_mode_symmetric_delay", False):
            return "runtime_metronome_mode_not_applied"
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
    if combined.get("metronome_beat_delta_abs_max", 0.0) > 2.0:
        return "metronome_grid_beat_delta_high"
    if server.get("metronome_mode", "") != expected_mode or client.get("metronome_mode", "") != expected_mode:
        return "metronome_mode_not_applied"
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
    analysis = match_clicks(expected, detected)
    analysis.update({
        "ok": analysis["missing_clicks"] == 0 and analysis["max_abs_error_frames"] <= METRONOME_WAV_TOLERANCE_FRAMES,
        "recording_dir": str(recording_dir),
        "sample_rate": sample_rate,
    })
    if not analysis["ok"]:
        analysis["verdict"] = (
            "metronome_click_count_mismatch"
            if analysis["missing_clicks"] > 0
            else "metronome_click_timing_high"
        )
    return analysis


def analyze_metronome_recordings(result, server_paths, client_paths):
    scenario = result.get("source_scenario") or result.get("scenario", "")
    if not (scenario == "metronome-shared-grid" or scenario.startswith("metronome-")):
        return {}
    allow_client_silent = scenario == "metronome-leader-audio"
    server = analyze_side_recording(Path(server_paths["dir"]) / "recording")
    client = analyze_side_recording(Path(client_paths["dir"]) / "recording", allow_silent=allow_client_silent)
    ok = server.get("ok", False) and client.get("ok", False)
    verdict = ""
    if not ok:
        verdict = server.get("verdict") if not server.get("ok", False) else client.get("verdict", "metronome_wav_failed")
    return {"ok": ok, "verdict": verdict, "server": server, "client": client}


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
        observations["server_audio_control_metronome_mode_symmetric_delay"] = text_contains(
            server_paths["stdout"],
            "Audio control metronome mode: symmetric-delay")
        observations["client_final_metronome_level_0_1"] = text_contains(
            client_paths["stdout"],
            "Final metronome level: 0.1")
        observations["client_final_remote_level_0_75"] = text_contains(
            client_paths["stdout"],
            "Final remote playback level: 0.75")
    return observations


def run_scenario(jam2, scenario_id, scenario, args_ns, output_dir, seed):
    profile = scenario["profile"]
    source_scenario = scenario.get("source_scenario", scenario_id)
    record_metronome = source_scenario == "metronome-shared-grid" or source_scenario.startswith("metronome-")
    commands = list(scenario.get("commands", []))
    if record_metronome:
        commands.extend([
            {"at_s": 1.0, "side": "server", "line": f"record jam start {Path(output_dir) / 'server' / 'recording'}"},
            {"at_s": 1.0, "side": "client", "line": f"record jam start {Path(output_dir) / 'client' / 'recording'}"},
            {"at_s": max(2.0, args_ns.stream_ms / 1000.0 - 1.0), "side": "server", "line": "record jam stop"},
            {"at_s": max(2.0, args_ns.stream_ms / 1000.0 - 1.0), "side": "client", "line": "record jam stop"},
        ])
    print_flush(f"[stress] starting {scenario_id} profile={profile.name}")
    listener, server_paths = start_listener(
        jam2,
        args_ns.server_audio_device,
        args_ns.sample_rate,
        profile,
        args_ns.stream_ms,
        output_dir,
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
        metrics = result.get("metrics", {}).get("combined", {})
        lines.append(
            f"{result.get('scenario')} verdict={result.get('verdict')} "
            f"loss_max={metrics.get('loss_percent_max', 0.0):.3f}% "
            f"jitter_max={metrics.get('jitter_max_ms', 0.0):.1f}ms "
            f"rtt_max={metrics.get('rtt_max_ms', 0.0):.1f}ms "
            f"underrun_ms={metrics.get('playback_underrun_time_ms_total', 0.0):.1f} "
            f"drift_abs_ppm={metrics.get('drift_abs_ppm_max', 0.0):.1f}"
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
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv else "",
    }


def run_validations(jam2, args_ns, logs):
    validations_dir = ensure_dir(logs / "validation")
    results = []
    results.append(validation_command(
        jam2,
        ["listen", "--definitely-bad-option"],
        validations_dir / "bad-option",
        1,
        "bad-option"))
    results.append(validation_command(
        jam2,
        ["connect", "not-a-jam-url", "--wait-ms", "100"],
        validations_dir / "bad-url",
        1,
        "bad-url"))
    results.append(validation_command(
        jam2,
        ["listen", "--bind", "127.0.0.1:0", "--no-stun", "--wait-ms", "250", "--machine-readable-startup", "on"],
        validations_dir / "listen-timeout",
        3,
        "listen-timeout"))
    fixed_session_profile = SAFE_LOCAL_PROFILE
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
        SAFE_LOCAL_PROFILE,
        SAFE_LOCAL_PROFILE,
        False,
        lambda url: replace_url_key(url, "ffffffffffffffffffffffffffffffff")))
    results.append(run_pair_validation(
        jam2,
        "mismatched-frame-size",
        args_ns,
        validations_dir / "mismatched-frame-size",
        SAFE_LOCAL_PROFILE,
        variant(SAFE_LOCAL_PROFILE, "frame_256", frame_size=256),
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

    logs = Path(args_ns.logs)
    if args_ns.clean and logs.exists():
        shutil.rmtree(logs)
    ensure_dir(logs)

    catalog, scenario_ids = scenario_plan(args_ns.profile, args_ns.scenario)
    unknown = [scenario for scenario in scenario_ids if scenario not in catalog]
    if unknown:
        return fail("unknown scenario(s): " + ", ".join(unknown))

    results = []
    for index, scenario_id in enumerate(scenario_ids):
        scenario_dir = ensure_dir(logs / f"{index + 1:02d}_{safe_test_id(scenario_id)}")
        write_json(scenario_dir / "scenario.json", {
            "scenario": scenario_id,
            "source_scenario": catalog[scenario_id].get("source_scenario", scenario_id),
            "profile_family": catalog[scenario_id].get("profile_family", args_ns.profile),
            "stream_ms": args_ns.stream_ms,
            "sample_rate": args_ns.sample_rate,
            "server_audio_device": args_ns.server_audio_device,
            "client_audio_device": args_ns.client_audio_device,
            "profile": catalog[scenario_id]["profile"].metadata(),
            "expect": catalog[scenario_id]["expect"],
        })
        result = run_scenario(jam2, scenario_id, catalog[scenario_id], args_ns, scenario_dir, args_ns.seed + index)
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
