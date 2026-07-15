#!/usr/bin/env python3

import argparse
import json
import secrets
import shutil
import socket
import subprocess
import threading
import time
import sys
import wave
from dataclasses import asdict
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_harness import (
    collect_side_csv,
    ManagedProcess,
    parse_endpoint,
    side_paths,
    start_connector,
    start_listener,
)
from jam2_metrics import combined_summary, summarize_csv, write_results_csv
from jam2_results import (
    analyze_listener_compensated_pulse,
    analyze_metronome_recordings,
    analyze_metro_pulse_epoch,
    mesh_collect_metrics,
    mesh_verdict,
    run_runtime_commands,
    scenario_observations,
    verdict_for,
)
from jam2_scenarios import (
    audio_probe_health_ok,
    mesh_peer_counts,
    mesh_scenario_plan,
    scenario_plan,
    selected_os_priorities,
)
from jam2_udp_protocol import parse_jam_url
from jam2_udp_validation import (
    NearWrapSequenceTransformer,
    OneShotAudioHeaderTransformer,
    PacketCapture,
    inject_delayed_replay,
    inject_malformed_corpus,
    inject_short_packet_flood,
)
from jam2_profiles import (
    FAST_PROFILE,
    MODERATE_PROFILE,
    SAFE_PROFILE,
    variant,
)
from jam2_tooling import (
    default_jam2_path,
    debug_description,
    ensure_dir,
    fail,
    new_run_manifest,
    print_flush,
    redact_cli_args,
    redact_structure,
    repo_root,
    safe_test_id,
    write_json,
)
from udp_stress_proxy import DirectionImpairment, ProxyImpairment, UdpStressProxy


DEFAULT_STREAM_MS = 30000
DEFAULT_HEADLESS_AUDIO_BUFFER_FRAMES = 1024
DEFAULT_SCENARIO_COOLDOWN_S = 2.0
def parse_args():
    parser = argparse.ArgumentParser(
        description="Run localhost Jam2 stress scenarios through a UDP impairment proxy or headless mesh peers.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--mode", choices=("normal", "mesh"), default="normal")
    parser.add_argument("--server-audio-device")
    parser.add_argument("--client-audio-device")
    parser.add_argument(
        "--headless-audio",
        action="store_true",
        help="use synthetic audio for normal create/join scenarios instead of physical devices")
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("stress_logs")))
    parser.add_argument("--stream-ms", type=int, default=DEFAULT_STREAM_MS)
    parser.add_argument("--startup-timeout-s", type=float, default=10.0)
    parser.add_argument(
        "--scenario-cooldown-s",
        type=float,
        default=DEFAULT_SCENARIO_COOLDOWN_S,
        help="device/process recovery time between scenarios; default: 2.0 seconds")
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
        default=0,
        help="first localhost UDP port used by headless mesh stress cases; 0 auto-selects available ports")
    parser.add_argument(
        "--headless-audio-buffer-frames",
        type=int,
        default=DEFAULT_HEADLESS_AUDIO_BUFFER_FRAMES,
        help="synthetic audio callback size for headless cases; default: 1024 frames")
    return parser.parse_args()


def select_mesh_endpoints(peer_count, base_port):
    reservations = []
    try:
        for index in range(peer_count):
            reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            port = base_port + index if base_port else 0
            try:
                reservation.bind(("127.0.0.1", port))
            except OSError as error:
                reservation.close()
                requested = f"127.0.0.1:{port}" if port else "an automatically selected localhost port"
                raise RuntimeError(f"mesh stress could not reserve {requested}: {error}") from error
            reservations.append(reservation)
        return [f"127.0.0.1:{reservation.getsockname()[1]}" for reservation in reservations]
    finally:
        for reservation in reservations:
            reservation.close()


_NETWORK_TEST_PORT_MIN = 20000
_NETWORK_TEST_PORT_MAX = 47999
_network_test_port_cursor = _NETWORK_TEST_PORT_MIN + secrets.randbelow(
    _NETWORK_TEST_PORT_MAX - _NETWORK_TEST_PORT_MIN + 1)


def select_network_endpoint(requested_port=0):
    """Choose one localhost number that is free for both coordinator TCP and audio UDP."""
    global _network_test_port_cursor
    candidate_count = 1 if requested_port else (_NETWORK_TEST_PORT_MAX - _NETWORK_TEST_PORT_MIN + 1)
    for _ in range(candidate_count):
        port = requested_port
        if not port:
            port = _network_test_port_cursor
            _network_test_port_cursor += 1
            if _network_test_port_cursor > _NETWORK_TEST_PORT_MAX:
                _network_test_port_cursor = _NETWORK_TEST_PORT_MIN
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            tcp.bind(("0.0.0.0", port))
            udp.bind(("127.0.0.1", port))
            return f"127.0.0.1:{port}"
        except OSError:
            continue
        finally:
            tcp.close()
            udp.close()
    requested = f" {requested_port}" if requested_port else ""
    raise RuntimeError(f"stress test could not reserve shared localhost TCP/UDP port{requested}")


def run_mesh_scenario(jam2, scenario_id, scenario, args_ns, output_dir, seed):
    profile = scenario["profile"]
    peer_count = scenario["mesh_peers"]
    os_priority = scenario.get("os_priority", "high")
    session_id = secrets.token_hex(8)
    session_key = secrets.token_hex(16)
    if args_ns.mesh_base_port:
        endpoints = [select_network_endpoint(args_ns.mesh_base_port)]
        endpoints.extend(select_mesh_endpoints(peer_count - 1, args_ns.mesh_base_port + 1))
    else:
        endpoints = [select_network_endpoint()]
        endpoints.extend(select_mesh_endpoints(peer_count - 1, 0))
    tokens = [secrets.token_hex(16) for _ in range(peer_count)]
    peer_endpoints = [
        {remote: endpoints[remote] for remote in range(peer_count) if remote != local}
        for local in range(peer_count)
    ]
    edge_proxies = []
    proxy_stop = threading.Event()
    for edge_index, edge in enumerate(scenario.get("edge_impairments", [])):
        peer_a = int(edge["peer_a"]) - 1
        peer_b = int(edge["peer_b"]) - 1
        if peer_a < 0 or peer_b < 0 or peer_a >= peer_count or peer_b >= peer_count or peer_a == peer_b:
            raise RuntimeError("mesh edge impairment peer indexes are invalid")
        proxy = UdpStressProxy(
            parse_endpoint(endpoints[peer_b]),
            impairment=edge["impairment"],
            seed=seed + edge_index)
        client_host, client_port = proxy.public_endpoint
        server_host, server_port = proxy.server_public_endpoint
        peer_endpoints[peer_a][peer_b] = f"{client_host}:{client_port}"
        peer_endpoints[peer_b][peer_a] = f"{server_host}:{server_port}"
        thread = threading.Thread(target=proxy.run_until, args=(proxy_stop,), daemon=True)
        thread.start()
        edge_proxies.append((proxy, thread, peer_a, peer_b))
    processes = []
    peer_results = []
    commands = list(scenario.get("commands", []))
    clock_drifts = list(scenario.get("headless_clock_drift_ppm", [0] * peer_count))
    if len(clock_drifts) != peer_count:
        raise RuntimeError("mesh scenario headless clock drift list must match its peer count")
    print_flush(f"[stress] starting {scenario_id} profile={profile.name} peers={peer_count} os_priority={os_priority}")

    topology = {
        tokens[local]: {
            tokens[remote]: peer_endpoints[local][remote]
            for remote in range(peer_count)
            if remote != local
        }
        for local in range(peer_count)
    }

    for index, endpoint in enumerate(endpoints):
        peer_name = f"peer{index + 1}"
        paths = side_paths(output_dir, peer_name)
        recording = ensure_dir(Path(paths["dir"]) / "recording")
        common_args = [
            "--sample-rate", str(args_ns.sample_rate),
            "--log-stats", str(paths["csv_raw"]),
            "--headless-audio", "on",
            "--test-input", scenario.get("signal", "tone-440"),
            "--record-jam-folder", str(recording),
            "--os-priority", os_priority,
        ]
        common_args.extend(profile.args(args_ns.stream_ms))
        common_args.extend(["--audio-buffer-size", str(args_ns.headless_audio_buffer_frames)])
        common_args.extend(["--headless-clock-drift-ppm", str(clock_drifts[index])])
        env = {
            "JAM2_DEBUG_SCENARIO": "1",
            "JAM2_DEBUG_PEER_TOKEN": tokens[index],
        }
        if index == 0:
            env["JAM2_DEBUG_TOPOLOGY"] = json.dumps(topology, separators=(",", ":"))
            args = [
                jam2,
                "network", "create",
                "--bind", endpoint,
                "--no-stun",
                "--session-id", session_id,
                "--session-key", session_key,
                "--wait-ms", str(max(15000, args_ns.stream_ms + 15000)),
            ] + common_args
        else:
            creator_startup = processes[0][0].wait_for_startup("waiting", args_ns.startup_timeout_s)
            if creator_startup is None:
                for process, _, _ in processes:
                    process.terminate()
                proxy_stop.set()
                for proxy, thread, _, _ in edge_proxies:
                    thread.join(timeout=2.0)
                    proxy.close()
                return {
                    "scenario": scenario_id,
                    "profile": profile.name,
                    "error": "mesh creator did not emit waiting startup JSON",
                    "peer_results": [],
                }
            args = [
                jam2,
                "network", "join", creator_startup["connection_url"],
                "--bind", endpoint,
                "--wait-ms", str(max(15000, args_ns.stream_ms + 15000)),
            ] + common_args
        process = ManagedProcess(
            args,
            repo_root(),
            paths["stdout"],
            paths["stderr"],
            stdin_pipe=bool(commands),
            env=env).start()
        processes.append((process, paths, peer_name))

    command_thread = None
    if commands:
        def run_mesh_commands():
            start = time.monotonic()
            targets = {peer_name: process for process, _, peer_name in processes}
            for command in sorted(commands, key=lambda item: item.get("at_s", 0.0)):
                delay = start + float(command.get("at_s", 0.0)) - time.monotonic()
                if delay > 0.0:
                    time.sleep(delay)
                target = targets.get(command.get("peer", ""))
                if target is not None:
                    target.send_line(command.get("line", ""))
        command_thread = threading.Thread(target=run_mesh_commands, daemon=True)
        command_thread.start()

    timeout = max(30.0, args_ns.stream_ms / 1000.0 + 30.0)
    for process, paths, peer_name in processes:
        rc = process.wait(timeout=timeout)
        if rc is None:
            process.terminate()
            rc = process.poll()
        csv_path = collect_side_csv(paths, process)
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
    if command_thread is not None:
        command_thread.join(timeout=1.0)

    proxy_stop.set()
    for proxy, thread, _, _ in edge_proxies:
        thread.join(timeout=2.0)
        proxy.close()
    edge_proxy_stats = [
        {
            "peer_a": peer_a + 1,
            "peer_b": peer_b + 1,
            "stats": dict(proxy.stats),
        }
        for proxy, _, peer_a, peer_b in edge_proxies
    ]

    mesh_metrics = mesh_collect_metrics(peer_results, args_ns.stream_ms)
    result = {
        "scenario": scenario_id,
        "source_scenario": scenario.get("source_scenario", scenario_id),
        "profile_family": scenario.get("profile_family", args_ns.profile),
        "profile": profile.name,
        "os_priority": os_priority,
        "expect": scenario["expect"],
        "profile_metadata": profile.metadata(),
        "requested_stream_ms": args_ns.stream_ms,
        "expected_early_client_exit": scenario.get("expected_early_client_exit", False),
        "headless_audio_buffer_frames": args_ns.headless_audio_buffer_frames,
        "mesh_peers": peer_count,
        "mesh_endpoints": endpoints,
        "mesh_peer_endpoints": peer_endpoints,
        "headless_clock_drift_ppm": clock_drifts,
        "edge_impairments": [
            {
                "peer_a": edge["peer_a"],
                "peer_b": edge["peer_b"],
                "impairment": asdict(edge["impairment"]),
            }
            for edge in scenario.get("edge_impairments", [])
        ],
        "edge_proxy_stats": edge_proxy_stats,
        "peer_results": peer_results,
        "mesh_metrics": mesh_metrics,
        "authority_peer": scenario.get("authority_peer", ""),
    }
    result["verdict"] = mesh_verdict(result)
    print_flush(
        f"[stress] finished {scenario_id} verdict={result['verdict']} "
        f"protocol={result.get('protocol_verdict')} "
        f"audio={result.get('audio_health_verdict')} "
        f"observations={','.join(result.get('audio_health_observations', [])) or '-'} "
        f"peers={peer_count} recv_min={mesh_metrics.get('recv_packets_min', 0.0):.0f} "
        f"loss={mesh_metrics.get('sequence_lost_total', 0.0):.0f} "
        f"active_edges_min={mesh_metrics.get('network_active_peer_count_established_min', mesh_metrics.get('network_active_peer_count_min', 0))}/"
        f"{mesh_metrics.get('expected_remote_peers', 0)} "
        f"mix_slots_min={mesh_metrics.get('mix_released_slots_min', 0.0):.0f} "
        f"mix_capacity_drops={mesh_metrics.get('mix_capacity_drops_total', 0.0):.0f} "
        f"drift_abs_max={mesh_metrics.get('drift_abs_ppm_max', 0.0):.1f}ppm "
        f"duration_coverage={result.get('duration_coverage_ratio_min', 0.0):.3f} "
        f"audio_ok={mesh_metrics.get('audio_ok_peers', 0)}/{peer_count}")
    return result


def run_scenario(jam2, scenario_id, scenario, args_ns, output_dir, seed):
    profile = scenario["profile"]
    os_priority = scenario.get("os_priority", "high")
    source_scenario = scenario.get("source_scenario", scenario_id)
    record_metronome = source_scenario == "metronome-shared-grid" or source_scenario.startswith("metronome-")
    audio_probe = source_scenario.startswith("audio-probe-")
    commands = list(scenario.get("commands", []))
    if scenario.get("prepared_track"):
        track_path = Path(output_dir) / "prepared-track.wav"
        track_path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(track_path), "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(args_ns.sample_rate)
            handle.writeframes(bytes(args_ns.sample_rate * 2))
        commands.extend([
            {"at_s": 1.0, "side": "server", "line": f"track load {track_path}"},
            {"at_s": 1.0, "side": "client", "line": f"track load {track_path}"},
        ])
    server_extra_args = ["--os-priority", os_priority]
    client_extra_args = ["--os-priority", os_priority]
    server_audio_device = args_ns.server_audio_device
    client_audio_device = args_ns.client_audio_device
    if args_ns.headless_audio:
        headless_args = [
            "--headless-audio", "on",
            "--audio-buffer-size", str(args_ns.headless_audio_buffer_frames),
        ]
        server_extra_args.extend(headless_args)
        client_extra_args.extend(headless_args)
        server_audio_device = None
        client_audio_device = None
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

    # Public create/join now keep the invitation endpoint on the TCP
    # coordinator. Route only the direct UDP edge through the impairment proxy
    # by using the inherited, bounded debug topology seam.
    server_bind = select_network_endpoint()
    real_endpoint = parse_endpoint(server_bind)
    server_token = secrets.token_hex(16)
    client_token = secrets.token_hex(16)
    udp_validation_kind = scenario.get("udp_validation", "")
    packet_capture = PacketCapture() if udp_validation_kind in ("malformed", "delayed-replay", "short-flood") else None
    proxy = UdpStressProxy(
        real_endpoint,
        impairment=scenario["impairment"],
        seed=seed,
        packet_observer=packet_capture.observe if packet_capture is not None else None)
    proxy_client = f"{proxy.public_endpoint[0]}:{proxy.public_endpoint[1]}"
    proxy_server = f"{proxy.server_public_endpoint[0]}:{proxy.server_public_endpoint[1]}"
    topology = {
        server_token: {client_token: proxy_server},
        client_token: {server_token: proxy_client},
    }
    server_env = {
        "JAM2_DEBUG_SCENARIO": "1",
        "JAM2_DEBUG_PEER_TOKEN": server_token,
        "JAM2_DEBUG_TOPOLOGY": json.dumps(topology, separators=(",", ":")),
    }
    client_env = {
        "JAM2_DEBUG_SCENARIO": "1",
        "JAM2_DEBUG_PEER_TOKEN": client_token,
    }
    stop_proxy = threading.Event()
    proxy_thread = threading.Thread(target=proxy.run_until, args=(stop_proxy,), daemon=True)
    proxy_thread.start()

    listener, server_paths = start_listener(
        jam2,
        server_audio_device,
        args_ns.sample_rate,
        profile,
        args_ns.stream_ms,
        output_dir,
        extra_args=server_extra_args,
        stdin_pipe=bool(commands),
        bind=server_bind,
        env=server_env)
    startup = listener.wait_for_startup("waiting", args_ns.startup_timeout_s)
    if startup is None:
        listener.terminate()
        stop_proxy.set()
        proxy_thread.join(timeout=2.0)
        proxy.close()
        return {
            "scenario": scenario_id,
            "profile": profile.name,
            "error": "listener did not emit waiting startup JSON",
            "server_return_code": listener.poll(),
            "client_return_code": None,
        }

    sequence_transformer = None
    if udp_validation_kind == "near-wrap-sequence":
        sequence_transformer = NearWrapSequenceTransformer(parse_jam_url(startup["connection_url"]).key)
    elif udp_validation_kind in ("forward-sequence-gap", "extreme-sample-time"):
        sequence_transformer = OneShotAudioHeaderTransformer(
            parse_jam_url(startup["connection_url"]).key,
            udp_validation_kind)
    proxy.packet_transformer = sequence_transformer

    connector, client_paths = start_connector(
        jam2,
        startup["connection_url"],
        client_audio_device,
        args_ns.sample_rate,
        profile,
        args_ns.stream_ms,
        output_dir,
        extra_args=client_extra_args,
        stdin_pipe=bool(commands),
        env=client_env)

    udp_validation = {}
    if udp_validation_kind == "malformed":
        udp_validation["injections"] = inject_malformed_corpus(proxy, packet_capture)
    elif udp_validation_kind == "delayed-replay":
        udp_validation["injections"] = inject_delayed_replay(proxy, packet_capture)
    elif udp_validation_kind == "short-flood":
        udp_validation["injections"] = inject_short_packet_flood(proxy, packet_capture)

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

    server_csv = collect_side_csv(server_paths, listener)
    client_csv = collect_side_csv(client_paths, connector)
    client_startups = connector.startup_payloads("connected")
    native_effective_config = {
        "server": redact_structure(startup),
        "client": redact_structure(client_startups[-1]) if client_startups else {},
    }
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
        "requested_stream_ms": args_ns.stream_ms,
        "expected_early_client_exit": scenario.get("expected_early_client_exit", False),
        "native_effective_config": native_effective_config,
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
    if sequence_transformer is not None:
        udp_validation["transformer"] = sequence_transformer.stats()
    if udp_validation:
        result["udp_validation"] = udp_validation
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
        f"protocol={result.get('protocol_verdict')} "
        f"audio={result.get('audio_health_verdict')} "
        f"observations={','.join(result.get('audio_health_observations', [])) or '-'} "
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
                f"protocol={result.get('protocol_verdict')} "
                f"duration={result.get('duration_verdict')} "
                f"audio_health={result.get('audio_health_verdict')} "
                f"audio_observations={','.join(result.get('audio_health_observations', [])) or '-'} "
                f"mesh_peers={metrics.get('peer_count', 0)} "
                f"recv_min={metrics.get('recv_packets_min', 0.0):.0f} "
                f"sent_min={metrics.get('sent_packets_min', 0.0):.0f} "
                f"loss_total={metrics.get('sequence_lost_total', 0.0):.0f} "
                f"jitter_max={metrics.get('jitter_max_ms', 0.0):.1f}ms "
                f"rtt_max={metrics.get('rtt_max_ms', 0.0):.1f}ms "
                f"underrun_ms={metrics.get('playback_underrun_time_ms_total', 0.0):.1f} "
                f"duration_coverage={result.get('duration_coverage_ratio_min', 0.0):.3f} "
                f"callback_avg_max={metrics.get('audio_callback_interval_avg_ms_max', 0.0):.2f}ms "
                f"callback_over_2x={metrics.get('audio_callback_gap_over_2x_total', 0.0):.0f} "
                f"stale_rejects={metrics.get('udp_sample_time_stale_rejects_total', 0.0):.0f} "
                f"jitter_forced={metrics.get('jitter_buffer_forced_releases_total', 0.0):.0f} "
                f"missing_frames={metrics.get('missing_audio_frames_total', 0.0):.0f} "
                f"dropped_frames={metrics.get('playback_dropped_frames_total', 0.0):.0f} "
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
                f" listener_pulse_max_abs_ms={combined.get('max_abs_error_ms_max', 0.0):.2f}"
                f" listener_pulse_steady_avg_abs_ms={combined.get('steady_avg_abs_error_ms_max', 0.0):.2f}"
                f" listener_pulse_steady_max_abs_ms={combined.get('steady_max_abs_error_ms_max', 0.0):.2f}")
        metro_pulse = result.get("metro_pulse_epoch_analysis", {})
        if metro_pulse:
            combined = metro_pulse.get("combined", {})
            pulse_suffix += (
                f" metro_pulse_epoch_ok={'yes' if metro_pulse.get('ok', False) else 'no'}"
                f" metro_pulse_epoch_max_error_frames={combined.get('max_abs_error_frames_max', 0)}")
        lines.append(
            f"{result.get('scenario')} verdict={result.get('verdict')} "
            f"protocol={result.get('protocol_verdict')} "
            f"duration={result.get('duration_verdict')} "
            f"audio_health={result.get('audio_health_verdict')} "
            f"audio_observations={','.join(result.get('audio_health_observations', [])) or '-'} "
            f"loss_max={metrics.get('loss_percent_max', 0.0):.3f}% "
            f"jitter_max={metrics.get('jitter_max_ms', 0.0):.1f}ms "
            f"rtt_max={metrics.get('rtt_max_ms', 0.0):.1f}ms "
            f"underrun_ms={metrics.get('playback_underrun_time_ms_total', 0.0):.1f} "
            f"duration_coverage={result.get('duration_coverage_ratio_min', 0.0):.3f} "
            f"callback_over_2x={metrics.get('audio_callback_gap_over_2x_total', 0.0):.0f} "
            f"jitter_forced={metrics.get('jitter_buffer_forced_releases_total', 0.0):.0f} "
            f"missing_frames={metrics.get('missing_audio_frames_total', 0.0):.0f} "
            f"dropped_frames={metrics.get('playback_dropped_frames_total', 0.0):.0f} "
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
        "args": redact_cli_args(args),
    }


def validation_help_command(jam2, args, output_dir, name, expected_text):
    result = validation_command(jam2, args, output_dir, 0, name)
    output = (Path(output_dir) / "stdout.txt").read_text(encoding="utf-8", errors="replace")
    missing = [text for text in expected_text if text not in output]
    result["missing_help_text"] = missing
    if result["verdict"] == "pass" and missing:
        result["verdict"] = "help_output_missing"
    return result


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
        "network", "create",
        "--bind", "127.0.0.1:0",
        "--no-stun",
        "--wait-ms", "250",
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
        "profile_args": redact_cli_args(profile_args),
        "explicit_args": redact_cli_args(explicit_args),
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
    server_bind = select_network_endpoint()
    real_endpoint = parse_endpoint(server_bind)
    server_token = secrets.token_hex(16)
    client_token = secrets.token_hex(16)
    proxy = UdpStressProxy(real_endpoint, impairment=ProxyImpairment.both(DirectionImpairment()), seed=args_ns.seed)
    topology = {
        server_token: {client_token: f"{proxy.server_public_endpoint[0]}:{proxy.server_public_endpoint[1]}"},
        client_token: {server_token: f"{proxy.public_endpoint[0]}:{proxy.public_endpoint[1]}"},
    }
    server_env = {
        "JAM2_DEBUG_SCENARIO": "1",
        "JAM2_DEBUG_PEER_TOKEN": server_token,
        "JAM2_DEBUG_TOPOLOGY": json.dumps(topology, separators=(",", ":")),
    }
    client_env = {
        "JAM2_DEBUG_SCENARIO": "1",
        "JAM2_DEBUG_PEER_TOKEN": client_token,
    }
    stop_proxy = threading.Event()
    proxy_thread = threading.Thread(target=proxy.run_until, args=(stop_proxy,), daemon=True)
    proxy_thread.start()
    listener, server_paths = start_listener(
        jam2,
        args_ns.server_audio_device,
        args_ns.sample_rate,
        server_profile,
        args_ns.validation_stream_ms,
        output_dir,
        extra_args=server_extra_args,
        bind=server_bind,
        env=server_env)
    startup = listener.wait_for_startup("waiting", args_ns.startup_timeout_s)
    if startup is None:
        listener.terminate()
        stop_proxy.set()
        proxy_thread.join(timeout=2.0)
        proxy.close()
        return {
            "validation": name,
            "verdict": "listener_startup_failed",
            "server_return_code": listener.poll(),
            "client_return_code": None,
        }

    url = startup["connection_url"]
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
        extra_args=client_extra_args,
        env=client_env)
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
    server_csv = collect_side_csv(server_paths, listener)
    client_csv = collect_side_csv(client_paths, connector)
    ok = (server_rc == 0 and client_rc == 0) if expect_success else (server_rc != 0 or client_rc != 0)
    return {
        "validation": name,
        "verdict": "pass" if ok else "unexpected_success" if not expect_success else "unexpected_failure",
        "server_return_code": server_rc,
        "client_return_code": client_rc,
        "server_args": listener.artifact_args(),
        "client_args": connector.artifact_args(),
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv else "",
    }


def validation_session_peer_limit(jam2, args_ns, output_dir):
    stream_ms = max(6000, args_ns.validation_stream_ms)
    bind = select_network_endpoint()
    common = ["--headless-audio", "on", "--audio-buffer-size", "256"]
    creator, creator_paths = start_listener(
        jam2,
        None,
        args_ns.sample_rate,
        FAST_PROFILE,
        stream_ms,
        output_dir / "creator",
        extra_args=common + ["--max-peers", "1"],
        bind=bind)
    waiting = creator.wait_for_startup("waiting", args_ns.startup_timeout_s)
    if waiting is None:
        creator.terminate()
        return {
            "validation": "session-peer-limit",
            "verdict": "creator_startup_failed",
            "creator_return_code": creator.poll(),
        }

    accepted, accepted_paths = start_connector(
        jam2,
        waiting["connection_url"],
        None,
        args_ns.sample_rate,
        FAST_PROFILE,
        stream_ms,
        output_dir / "accepted",
        extra_args=common)
    accepted_startup = accepted.wait_for_startup("connected", args_ns.startup_timeout_s)
    if accepted_startup is None:
        accepted.terminate()
        creator.terminate()
        return {
            "validation": "session-peer-limit",
            "verdict": "first_peer_not_accepted",
            "creator_return_code": creator.poll(),
            "accepted_return_code": accepted.poll(),
        }

    rejected, rejected_paths = start_connector(
        jam2,
        waiting["connection_url"],
        None,
        args_ns.sample_rate,
        FAST_PROFILE,
        stream_ms,
        output_dir / "rejected",
        extra_args=common)
    rejected_rc = rejected.wait(timeout=10.0)
    if rejected_rc is None:
        rejected.terminate()
        rejected_rc = rejected.poll()
    accepted_rc = accepted.wait(timeout=stream_ms / 1000.0 + 15.0)
    if accepted_rc is None:
        accepted.terminate()
        accepted_rc = accepted.poll()
    creator_rc = creator.wait(timeout=10.0)
    if creator_rc is None:
        creator.terminate()
        creator_rc = creator.poll()

    creator_text = Path(creator_paths["stdout"]).read_text(encoding="utf-8", errors="replace")
    rejected_text = (
        Path(rejected_paths["stdout"]).read_text(encoding="utf-8", errors="replace") +
        Path(rejected_paths["stderr"]).read_text(encoding="utf-8", errors="replace"))
    ok = (
        creator_rc == 0 and accepted_rc == 0 and rejected_rc not in (None, 0) and
        "Session peer limit reached" in creator_text and
        "Session peer limit reached" in rejected_text)
    return {
        "validation": "session-peer-limit",
        "verdict": "pass" if ok else "peer_limit_not_enforced",
        "creator_return_code": creator_rc,
        "accepted_return_code": accepted_rc,
        "rejected_return_code": rejected_rc,
        "creator_args": creator.artifact_args(),
        "accepted_args": accepted.artifact_args(),
        "rejected_args": rejected.artifact_args(),
        "creator_csv_path": str(collect_side_csv(creator_paths, creator) or ""),
        "accepted_csv_path": str(collect_side_csv(accepted_paths, accepted) or ""),
    }


def run_validations(jam2, args_ns, logs):
    validations_dir = ensure_dir(logs / "validation")
    results = []
    help_dir = ensure_dir(validations_dir / "cli-help")
    for name, command, expected in (
            ("root", ["-h"], ["Commands:", "network create", "debug run"]),
            ("list-devices", ["list-devices", "-h"], ["Usage:", "jam2 list-devices"]),
            ("test-device", ["test-device", "-h"], ["--sample-rate", "<id>"]),
            ("meter-device", ["meter-device", "-h"], ["--buffer-size", "--duration-ms"]),
            ("ring-device", ["ring-device", "-h"], ["--ring-frames", "--duration-ms"]),
            ("local", ["local", "-h"], ["--audio-device", "--headless-audio", "--profile"]),
            ("network", ["network", "-h"], ["create", "join"]),
            ("network-create", ["network", "create", "-h"], ["--stun", "--no-stun", "--bind"]),
            ("network-join", ["network", "join", "-h"], ["<jam2-url>", "--wait-ms", "--bind"]),
            ("debug", ["debug", "-h"], ["describe", "run"]),
            ("debug-describe", ["debug", "describe", "-h"], ["--json", "automation schema"]),
            ("debug-run", ["debug", "run", "-h"], ["scenario.json", "262144", "Secrets"]),
    ):
        results.append(validation_help_command(
            jam2, command, help_dir / name, f"cli-help-{name}", expected))
    debug_dir = ensure_dir(validations_dir / "debug-adapter")
    results.append(validation_command(
        jam2,
        ["debug", "run", str(repo_root() / "tools" / "scenarios" / "local-headless-smoke.json")],
        debug_dir / "valid-local-smoke",
        0,
        "debug-valid-local-smoke"))
    results.append(validation_command(
        jam2,
        ["debug", "run", str(repo_root() / "tools" / "scenarios" / "lifecycle-headless-smoke.json")],
        debug_dir / "valid-lifecycle-smoke",
        0,
        "debug-valid-lifecycle-smoke"))
    results.append(validation_command(
        jam2,
        ["debug", "run", str(
            repo_root() / "tools" / "scenarios" / "controller-lifecycle-validation.json")],
        debug_dir / "valid-controller-lifecycle",
        0,
        "debug-valid-controller-lifecycle"))

    boundary_fixtures = ensure_dir(debug_dir / "boundary-fixtures")
    valid_wav = boundary_fixtures / "valid.wav"
    with wave.open(str(valid_wav), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(48000)
        handle.writeframes(bytes(960))
    truncated_wav = boundary_fixtures / "truncated.wav"
    truncated_wav.write_bytes(b"RIFF\x20\x00\x00\x00WAVEfmt ")
    bad_align_wav = boundary_fixtures / "bad-block-align.wav"
    bad_align = bytearray(valid_wav.read_bytes())
    bad_align[32:34] = (3).to_bytes(2, "little")
    bad_align_wav.write_bytes(bad_align)
    trailing_wav = boundary_fixtures / "trailing-data.wav"
    trailing_wav.write_bytes(valid_wav.read_bytes() + b"unexpected")
    boundary_scenario = debug_dir / "boundary-validation.json"
    write_json(boundary_scenario, {
        "schema": "jam2-debug-scenario-v1",
        "run_id": "boundary-validation",
        "operation": "validate.boundaries",
        "arguments": [
            "valid:" + str(valid_wav.resolve()),
            "invalid:" + str(truncated_wav.resolve()),
            "invalid:" + str(bad_align_wav.resolve()),
            "invalid:" + str(trailing_wav.resolve()),
        ],
    })
    results.append(validation_command(
        jam2, ["debug", "run", str(boundary_scenario)],
        debug_dir / "valid-boundary-suite", 0, "debug-valid-boundary-suite"))

    malformed_scenario = debug_dir / "malformed.json"
    malformed_scenario.write_text("{", encoding="utf-8")
    results.append(validation_command(
        jam2, ["debug", "run", str(malformed_scenario)],
        debug_dir / "reject-malformed", 2, "debug-reject-malformed"))

    unknown_field_scenario = debug_dir / "unknown-field.json"
    write_json(unknown_field_scenario, {
        "schema": "jam2-debug-scenario-v1",
        "run_id": "unknown-field",
        "operation": "local",
        "arguments": ["--headless-audio", "on"],
        "unexpected": True,
    })
    results.append(validation_command(
        jam2, ["debug", "run", str(unknown_field_scenario)],
        debug_dir / "reject-unknown-field", 2, "debug-reject-unknown-field"))

    secret_scenario = debug_dir / "secret-argument.json"
    write_json(secret_scenario, {
        "schema": "jam2-debug-scenario-v1",
        "run_id": "secret-argument",
        "operation": "local",
        "arguments": ["--headless-audio", "on", "--session-key"],
    })
    results.append(validation_command(
        jam2, ["debug", "run", str(secret_scenario)],
        debug_dir / "reject-secret-argument", 2, "debug-reject-secret-argument"))

    secret_assignment_scenario = debug_dir / "secret-assignment.json"
    write_json(secret_assignment_scenario, {
        "schema": "jam2-debug-scenario-v1",
        "run_id": "secret-assignment",
        "operation": "local",
        "arguments": ["--headless-audio", "on", "--session-key=not-persistable"],
    })
    results.append(validation_command(
        jam2, ["debug", "run", str(secret_assignment_scenario)],
        debug_dir / "reject-secret-assignment", 2, "debug-reject-secret-assignment"))

    excessive_arguments_scenario = debug_dir / "excessive-arguments.json"
    write_json(excessive_arguments_scenario, {
        "schema": "jam2-debug-scenario-v1",
        "run_id": "excessive-arguments",
        "operation": "local",
        "arguments": ["x"] * 129,
    })
    results.append(validation_command(
        jam2, ["debug", "run", str(excessive_arguments_scenario)],
        debug_dir / "reject-excessive-arguments", 2, "debug-reject-excessive-arguments"))

    oversized_scenario = debug_dir / "oversized.json"
    oversized_scenario.write_text("{" + (" " * (256 * 1024)) + "}", encoding="utf-8")
    results.append(validation_command(
        jam2, ["debug", "run", str(oversized_scenario)],
        debug_dir / "reject-oversized", 2, "debug-reject-oversized"))

    results.append(validation_command(
        jam2,
        ["network", "create", "--profile", "fast", "--definitely-bad-option"],
        validations_dir / "bad-option",
        2,
        "bad-option"))
    results.append(validation_command(
        jam2,
        ["network", "join", "not-a-jam-url", "--profile", "fast", "--wait-ms", "100"],
        validations_dir / "bad-url",
        2,
        "bad-url"))
    results.append(validation_command(
        jam2,
        ["network", "create", "--profile", "fast", "--bind", "127.0.0.1:0", "--no-stun", "--wait-ms", "250"],
        validations_dir / "listen-timeout",
        3,
        "listen-timeout"))
    refused_dir = validations_dir / "initial-connect-refused"
    refused = validation_command(
        jam2,
        [
            "network", "join",
            "jam2://v1?endpoint=127.0.0.1:1&session=1&key=00000000000000000000000000000001",
            "--profile", "fast", "--bind", "127.0.0.1:0", "--wait-ms", "6000",
        ],
        refused_dir,
        3,
        "initial-connect-refused")
    refused_output = (
        (refused_dir / "stdout.txt").read_text(encoding="utf-8", errors="replace") +
        (refused_dir / "stderr.txt").read_text(encoding="utf-8", errors="replace"))
    if refused["verdict"] == "pass" and "TCP control transport error:" not in refused_output:
        refused["verdict"] = "typed_transport_failure_missing"
    results.append(refused)
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
    results.append(validation_session_peer_limit(
        jam2,
        args_ns,
        validations_dir / "session-peer-limit"))
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
    if args_ns.scenario_cooldown_s < 0.0:
        return fail("--scenario-cooldown-s cannot be negative")
    if args_ns.headless_audio_buffer_frames <= 0:
        return fail("--headless-audio-buffer-frames must be positive")
    if (args_ns.mode == "normal" and not args_ns.headless_audio and
            (not args_ns.server_audio_device or not args_ns.client_audio_device)):
        return fail("--server-audio-device and --client-audio-device are required in --mode normal")

    logs = Path(args_ns.logs)
    if args_ns.clean and logs.exists():
        shutil.rmtree(logs)
    ensure_dir(logs)

    try:
        capabilities = debug_description(jam2)
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        return fail(f"jam2 debug capability query failed: {error}")

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

    write_json(logs / "run_manifest.json", new_run_manifest(
        "run_stress_local.py",
        sys.argv,
        jam2=str(jam2),
        mode=args_ns.mode,
        sample_rate=args_ns.sample_rate,
        stream_ms=args_ns.stream_ms,
        scenario_cooldown_s=args_ns.scenario_cooldown_s,
        headless_audio=args_ns.headless_audio,
        headless_audio_buffer_frames=args_ns.headless_audio_buffer_frames,
        seed=args_ns.seed,
        profile=args_ns.profile,
        scenarios=[planned_id for planned_id, _, _ in run_plan],
        server_audio_device=args_ns.server_audio_device,
        client_audio_device=args_ns.client_audio_device,
        native_debug_description=capabilities))

    for index, (planned_id, scenario_id, os_priority) in enumerate(run_plan):
        scenario_dir = ensure_dir(logs / f"{index + 1:02d}_{safe_test_id(planned_id)}")
        scenario = dict(catalog[scenario_id])
        scenario.setdefault("source_scenario", scenario_id)
        scenario["os_priority"] = os_priority
        write_json(scenario_dir / "scenario.json", {
            "schema_version": 1,
            "scenario": planned_id,
            "source_scenario": scenario.get("source_scenario", scenario_id),
            "profile_family": scenario.get("profile_family", args_ns.profile),
            "mode": args_ns.mode,
            "stream_ms": args_ns.stream_ms,
            "sample_rate": args_ns.sample_rate,
            "headless_audio": args_ns.headless_audio,
            "headless_audio_buffer_frames": (
                args_ns.headless_audio_buffer_frames
                if args_ns.mode == "mesh" or args_ns.headless_audio else 0),
            "server_audio_device": args_ns.server_audio_device,
            "client_audio_device": args_ns.client_audio_device,
            "seed": args_ns.seed + index,
            "mesh_peers": scenario.get("mesh_peers", 0),
            "headless_clock_drift_ppm": scenario.get("headless_clock_drift_ppm", []),
            "edge_impairments": [
                {
                    "peer_a": edge["peer_a"],
                    "peer_b": edge["peer_b"],
                    "impairment": asdict(edge["impairment"]),
                }
                for edge in scenario.get("edge_impairments", [])
            ],
            "profile": scenario["profile"].metadata(),
            "impairment": asdict(scenario["impairment"]) if "impairment" in scenario else {},
            "os_priority": os_priority,
            "expect": scenario["expect"],
            "signal": scenario.get("signal", ""),
            "server_signal": scenario.get("server_signal", ""),
            "client_signal": scenario.get("client_signal", ""),
            "udp_validation": scenario.get("udp_validation", ""),
            "expected_early_client_exit": scenario.get("expected_early_client_exit", False),
        })
        if args_ns.mode == "mesh":
            result = run_mesh_scenario(jam2, planned_id, scenario, args_ns, scenario_dir, args_ns.seed + index)
        else:
            result = run_scenario(jam2, planned_id, scenario, args_ns, scenario_dir, args_ns.seed + index)
        write_json(scenario_dir / "result.json", result)
        results.append(result)
        if index + 1 < len(run_plan) and args_ns.scenario_cooldown_s > 0.0:
            time.sleep(args_ns.scenario_cooldown_s)

    write_json(logs / "stress_results.json", {"schema_version": 1, "results": results})
    write_results_csv(logs / "stress_results.csv", results)
    write_summary(logs / "stress_summary.txt", results)
    validation_results = []
    if args_ns.include_validation:
        validation_results = run_validations(jam2, args_ns, logs)
        write_json(logs / "validation_results.json", {"schema_version": 1, "results": validation_results})
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
