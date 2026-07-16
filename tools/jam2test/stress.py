from __future__ import annotations

import json
import secrets
import socket
import subprocess
import threading
import time
import wave
from dataclasses import asdict
from pathlib import Path
from typing import Any

from .artifacts import InvocationArtifacts, normalized_path_id
from .audio_analysis import analyze_recording_dir
from .format_comparison import write_format_comparison
from .manifest import InvocationManifest
from .metrics import combined_summary, summarize_csv, write_results_csv
from .native import NativeCapabilities, native_manifest, write_scenario
from .profiles import configure_native_profiles
from .results import (
    analyze_listener_compensated_pulse, analyze_metronome_recordings,
    analyze_metro_pulse_epoch, mesh_collect_metrics, mesh_verdict,
    verdict_for,
)
from .udp_protocol import parse_jam_url
from .udp_validation import (
    NearWrapSequenceTransformer, OneShotAudioHeaderTransformer, PacketCapture,
    inject_delayed_replay, inject_malformed_corpus, inject_short_packet_flood,
)
from .impairment import UdpStressProxy


def _reserve_ports(count: int) -> list[int]:
    reservations: list[socket.socket] = []
    try:
        for _ in range(count):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            reservations.append(sock)
        return [sock.getsockname()[1] for sock in reservations]
    finally:
        for sock in reservations: sock.close()


def _selected_ports(count: int, base_port: int) -> list[int]:
    if base_port == 0:
        return _reserve_ports(count)
    if base_port < 1 or base_port + count - 1 > 65535:
        raise ValueError("--mesh-base-port does not leave room for every peer")
    reservations: list[socket.socket] = []
    tcp: socket.socket | None = None
    try:
        for port in range(base_port, base_port + count):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", port))
            reservations.append(sock)
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.bind(("127.0.0.1", base_port))
        return list(range(base_port, base_port + count))
    finally:
        if tcp is not None:
            tcp.close()
        for sock in reservations:
            sock.close()


def _reserve_creator_port() -> int:
    for _ in range(128):
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            udp.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            tcp.close(); udp.close()
    raise RuntimeError("could not reserve a creator TCP/UDP port")


def _action(line: str, at_s: float, sample_rate: int, action_id: str) -> dict[str, Any]:
    parts = line.split()
    base: dict[str, Any] = {
        "id": action_id,
        "after_event": "network.connected",
        "delay_frames": round(at_s * sample_rate),
    }
    if parts[0] == "bpm": return {**base, "type": "metronome.bpm", "value": int(parts[1])}
    if parts[:1] == ["metro"]:
        if parts[1] in ("on", "off"): return {**base, "type": "metronome.enabled", "enabled": parts[1] == "on"}
        if parts[1] == "mode": return {**base, "type": "metronome.mode", "mode": parts[2]}
        if parts[1] == "level": return {**base, "type": "metronome.level", "value": float(parts[2])}
    if parts[:1] == ["remote"]:
        value = 0.0 if parts[1] == "mute" else 1.0 if parts[1] == "unmute" else float(parts[2])
        return {**base, "type": "remote.level", "value": value}
    if parts[:2] == ["track", "sync"]:
        return {**base, "type": "track.sync", "enabled": parts[2] == "on"}
    if parts[:2] == ["track", "load"]:
        return {**base, "type": "track.load", "path": line[len("track load "):]}
    if parts[:2] == ["track", "play"]: return {**base, "type": "track.play"}
    if parts[:2] == ["track", "stop"]: return {**base, "type": "track.stop"}
    if parts[:2] == ["track", "restart"]: return {**base, "type": "track.restart"}
    if parts[:2] == ["track", "record-start"]:
        return {**base, "type": "track.record-start", "count_in_bars": int(parts[2]) if len(parts) > 2 else 1}
    if parts[:3] == ["record", "jam", "start"]:
        return {**base, "type": "recording.start", "path": line[len("record jam start "):]}
    if parts[:3] == ["record", "jam", "stop"]: return {**base, "type": "recording.stop"}
    if parts[0] in ("stats", "status"): return {**base, "type": "snapshot"}
    if parts[0] in ("quit", "exit"): return {**base, "type": "shutdown"}
    raise ValueError(f"retained stress action has no typed native mapping: {line}")


def _actions_for_peer(scenario: dict[str, Any], peer_index: int, sample_rate: int,
                      recording: Path | None, prepared_track: Path | None,
                      stream_ms: int) -> list[dict[str, Any]]:
    aliases = {0: {"server", "peer1"}, 1: {"client", "peer2"}}
    names = aliases.get(peer_index, {f"peer{peer_index + 1}"})
    actions = []
    if prepared_track:
        actions.append(_action(f"track load {prepared_track}", 1.0, sample_rate, f"load-{peer_index}"))
    for number, command in enumerate(scenario.get("commands", [])):
        side = command.get("side") or command.get("peer")
        if side in names:
            actions.append(_action(command["line"], float(command["at_s"]), sample_rate, f"catalog-{peer_index}-{number}"))
    if recording:
        actions.append(_action(f"record jam start {recording}", 0.0, sample_rate, f"record-start-{peer_index}"))
        actions.append(_action("record jam stop", stream_ms / 1000.0, sample_rate, f"record-stop-{peer_index}"))
    # Let the creator leave first so joiners observe normal authenticated peer
    # removal and clear departed playback state before their own scheduled
    # shutdown. This also retains the authority-handoff evidence expected by
    # the migrated stress catalog without Python wall-clock control actions.
    shutdown_s = stream_ms / 1000.0 + (1.0 if peer_index != 0 else 0.5)
    actions.append(_action("quit", shutdown_s, sample_rate, f"shutdown-{peer_index}"))
    return actions


def _csv_from_manifest(peer_root: Path, manifest: dict[str, Any]) -> Path | None:
    for artifact in manifest.get("artifacts", []):
        path = peer_root / artifact.get("path", "")
        if path.suffix.lower() == ".csv" and path.is_file(): return path
    return None


def _manifest_peer_id(manifest: dict[str, Any], fallback: Any = 0) -> int:
    value = manifest.get("local_peer_id")
    if value in (None, ""):
        value = fallback
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _wait(process: subprocess.Popen[str], timeout: float) -> tuple[int, bool]:
    try:
        return process.wait(timeout=timeout), False
    except subprocess.TimeoutExpired:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
        return process.returncode, True


def _effective_stream_ms(scenario: dict[str, Any], requested_stream_ms: int) -> int:
    """Return the bounded duration needed to exercise the selected case.

    ``--stream-ms`` remains the caller's requested minimum. Retained catalog
    cases may contain later native actions or an impairment/recovery window
    which cannot truthfully be assessed in a shorter run.
    """
    minimum_ms = 0
    minimum_ms = max(minimum_ms, int(scenario.get("minimum_stream_ms", 0)))
    commands = scenario.get("commands", [])
    if commands:
        minimum_ms = max(
            minimum_ms,
            round((max(float(item.get("at_s", 0.0)) for item in commands) + 2.0) * 1000.0),
        )
    impairment = scenario.get("impairment")
    if impairment is not None:
        for direction in (impairment.client_to_server, impairment.server_to_client):
            if direction.burst_every_ms > 0.0:
                minimum_ms = max(
                    minimum_ms,
                    round(direction.burst_every_ms + direction.burst_pause_ms + 2000.0),
                )
    source = scenario.get("source_scenario", "")
    if source in ("transient-stall-recovery", "transient-stall-250-recovery"):
        minimum_ms = max(minimum_ms, 18000)
    if (source.startswith("metronome-listener-compensated-pulse") or
            source == "metronome-listener-compensated-metro-pulse"):
        minimum_ms = max(minimum_ms, 12000)
    return max(int(requested_stream_ms), minimum_ms)


def _run_case(jam2: Path, case_id: str, scenario: dict[str, Any], args: Any,
              case_root: Path, seed: int) -> dict[str, Any]:
    peer_count = int(scenario.get("mesh_peers", 2))
    ports = _selected_ports(peer_count, args.mesh_base_port)
    if args.mesh_base_port == 0:
        ports[0] = _reserve_creator_port()
    session_id = f"{secrets.randbits(64) or 1:016x}"
    session_key = secrets.token_hex(16)
    invite = f"jam2://v1?endpoint=127.0.0.1:{ports[0]}&session={session_id}&key={session_key}"
    tokens = [secrets.token_hex(16) for _ in range(peer_count)]
    profile = scenario["profile"]
    effective_stream_ms = _effective_stream_ms(scenario, args.stream_ms)
    peer_profiles = list(scenario.get("peer_profiles", [profile] * peer_count))
    if len(peer_profiles) != peer_count:
        raise ValueError("peer_profiles must contain one profile per peer")
    prepared_track = None
    if scenario.get("prepared_track"):
        prepared_track = case_root / "prepared-track.wav"
        with wave.open(str(prepared_track), "wb") as handle:
            handle.setnchannels(1); handle.setsampwidth(2); handle.setframerate(args.sample_rate)
            handle.writeframes(bytes(args.sample_rate * 2))

    proxies: list[tuple[UdpStressProxy, threading.Event, threading.Thread, dict[str, Any]]] = []
    topology: dict[str, dict[str, str]] = {token: {} for token in tokens}
    edge_specs = scenario.get("edge_impairments", [])
    if peer_count == 2 and "impairment" in scenario:
        edge_specs = [{"peer_a": 1, "peer_b": 2, "impairment": scenario["impairment"]}]
    udp_kind = scenario.get("udp_validation", "")
    capture = PacketCapture() if udp_kind in ("malformed", "delayed-replay", "short-flood") else None
    transformer = None
    if udp_kind == "near-wrap-sequence": transformer = NearWrapSequenceTransformer(bytes.fromhex(session_key))
    elif udp_kind in ("forward-sequence-gap", "extreme-sample-time"):
        transformer = OneShotAudioHeaderTransformer(bytes.fromhex(session_key), udp_kind)
    for edge_index, edge in enumerate(edge_specs):
        a = int(edge["peer_a"]) - 1; b = int(edge["peer_b"]) - 1
        proxy = UdpStressProxy(("127.0.0.1", ports[a]), impairment=edge["impairment"],
                               seed=seed + edge_index,
                               packet_observer=capture.observe if capture and edge_index == 0 else None,
                               packet_transformer=transformer if edge_index == 0 else None)
        topology[tokens[a]][tokens[b]] = f"{proxy.server_public_endpoint[0]}:{proxy.server_public_endpoint[1]}"
        topology[tokens[b]][tokens[a]] = f"{proxy.public_endpoint[0]}:{proxy.public_endpoint[1]}"
        stop = threading.Event()
        thread = threading.Thread(target=proxy.run_until, args=(stop,), daemon=True)
        thread.start()
        proxies.append((proxy, stop, thread, edge))

    source_case = scenario.get("source_scenario", case_id)
    record = (peer_count > 2 or source_case.startswith("audio-probe-") or
              source_case.startswith("metronome-"))
    scenario_paths: list[tuple[Path, Path]] = []
    clock_drifts = scenario.get("headless_clock_drift_ppm", [0] * peer_count)
    for index in range(peer_count):
        peer_root = case_root / f"peer-{index + 1}"
        peer_root.mkdir(parents=True)
        operation = "network.create" if index == 0 else "network.join"
        peer_profile = peer_profiles[index]
        runtime = dict(peer_profile.runtime(effective_stream_ms))
        # Retained stress cases deliberately run a shared grid. These are test
        # settings, not copied native profile defaults.
        runtime.setdefault("metronome", True)
        runtime.setdefault("bpm", 120)
        runtime.setdefault("metronome_level", 0.20)
        runtime.update({
            "sample_rate": args.sample_rate,
            "network_audio_format": scenario.get("network_audio_format", "pcm24"),
            "stats_interval_ms": 1000,
            "os_priority": scenario.get("os_priority", "high"),
            "test_input": scenario.get("server_signal" if index == 0 else "client_signal", scenario.get("signal", "silence")),
            "headless_clock_drift_ppm": clock_drifts[index] if index < len(clock_drifts) else 0,
            "stream_ms": 0,
        })
        device = args.server_audio_device if index == 0 else args.client_audio_device
        if device is None:
            runtime.update({
                "headless_audio": True,
                # Keep one stable synthetic callback cadence. The asymmetric
                # cases compare the real local receive/playout profile values;
                # sub-millisecond native callback sizes are not wall-clock
                # accurate under the Windows test scheduler.
                "audio_buffer_size": args.headless_audio_buffer_frames,
            })
        else:
            runtime["audio_device"] = device
        recording = peer_root / "recording" if record else None
        actions = _actions_for_peer(
            scenario, index, args.sample_rate, recording, prepared_track, effective_stream_ms)
        native_scenario = {
            "schema": "jam2-debug-scenario", "run_id": f"{case_id}-peer-{index + 1}",
            "operation": operation, "profile": peer_profile.base_profile,
            "runtime": runtime, "artifacts": {"root": str(peer_root)},
            "network": {
                "bind": f"127.0.0.1:{ports[index]}", "no_stun": True,
                "wait_ms": max(10000, effective_stream_ms + 10000),
                "peer_token": tokens[index], "topology": topology,
            },
            "actions": actions,
        }
        if prepared_track: native_scenario["fixtures"] = [str(prepared_track)]
        if index == 0:
            native_scenario["network"].update({"session_id": session_id, "session_key": session_key, "max_peers": peer_count})
        else:
            native_scenario["network"]["join_url"] = invite
        path = peer_root / "scenario.json"
        write_scenario(path, native_scenario)
        scenario_paths.append((path, peer_root))

    processes: list[tuple[subprocess.Popen[str], Any, Any, Path]] = []
    injection: dict[str, Any] = {}
    try:
        for index, (path, peer_root) in enumerate(scenario_paths):
            stdout = (peer_root / "stdout.log").open("w", encoding="utf-8", newline="")
            stderr = (peer_root / "stderr.log").open("w", encoding="utf-8", newline="")
            process = subprocess.Popen([str(jam2), "debug", "run", str(path)], stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, text=True)
            processes.append((process, stdout, stderr, peer_root))
            if index == 0: time.sleep(0.15)
        if proxies and capture:
            proxy = proxies[0][0]
            if udp_kind == "malformed": injection["injections"] = inject_malformed_corpus(proxy, capture)
            elif udp_kind == "delayed-replay": injection["injections"] = inject_delayed_replay(proxy, capture)
            elif udp_kind == "short-flood": injection["injections"] = inject_short_packet_flood(proxy, capture)
        peer_results = []
        for process, stdout, stderr, peer_root in processes:
            code, timed_out = _wait(
                process,
                max(5.0, effective_stream_ms / 1000.0 + args.startup_timeout_s + 5.0),
            )
            stdout.close(); stderr.close()
            try:
                native = native_manifest(peer_root)
                csv_path = _csv_from_manifest(peer_root, native)
            except Exception as error:
                native = {"error": str(error)}; csv_path = None
            csv_summary = summarize_csv(
                csv_path if csv_path is not None else peer_root / "missing.csv",
                assessment_elapsed_ms=effective_stream_ms,
            )
            audio = analyze_recording_dir(peer_root / "recording", scenario.get("signal", "silence")) if (peer_root / "recording").exists() else {}
            peer_results.append({
                "process_id": peer_root.name,
                "role": "coordinator" if len(peer_results) == 0 else "peer",
                "local_peer_id": _manifest_peer_id(native, csv_summary.get("local_peer_id", 0)),
                "remote_peer_id": csv_summary.get("remote_peer_id", 0),
                "return_code": code, "timed_out": timed_out,
                "native_manifest": native, "csv_path": str(csv_path) if csv_path else "",
                "csv_summary": csv_summary,
                "audio_analysis": audio,
                "local_profile": peer_profiles[len(peer_results)].base_profile,
            })
    finally:
        for process, stdout, stderr, _ in processes:
            if process.poll() is None: process.kill(); process.wait()
            if not stdout.closed: stdout.close()
            if not stderr.closed: stderr.close()
        for proxy, stop, thread, _ in proxies:
            stop.set(); thread.join(timeout=2)

    edge_stats = []
    for proxy, _, _, edge in proxies:
        edge_stats.append({"peer_a": edge["peer_a"], "peer_b": edge["peer_b"], "stats": dict(proxy.stats)})
        proxy.close()
    result: dict[str, Any] = {
        "scenario": case_id, "source_scenario": scenario.get("source_scenario", case_id),
        "requested_network_audio_format": scenario.get("network_audio_format", "pcm24"),
        "profile": profile.name, "base_profile": profile.base_profile,
        "sparse_overrides": profile.overrides,
        "session_profile": peer_profiles[0].base_profile,
        "local_profiles": [item.base_profile for item in peer_profiles],
        "session_frame_size": peer_profiles[0].frame_size,
        "requested_stream_ms": args.stream_ms,
        "effective_stream_ms": effective_stream_ms,
        "expect": scenario["expect"], "peers": peer_results,
        "expected_early_client_exit": bool(scenario.get("expected_early_client_exit", False)),
        "headless_clock_drift_ppm": clock_drifts, "edge_impairments": [
            {**{k: v for k, v in edge.items() if k != "impairment"}, "impairment": asdict(edge["impairment"])}
            for edge in edge_specs], "edge_proxy_stats": edge_stats,
    }
    if transformer: injection["transformer"] = transformer.stats()
    if injection: result["udp_validation"] = injection
    if peer_count == 2:
        server_csv = Path(peer_results[0]["csv_path"]) if peer_results[0]["csv_path"] else None
        client_csv = Path(peer_results[1]["csv_path"]) if peer_results[1]["csv_path"] else None
        result["server_return_code"] = peer_results[0]["return_code"]
        result["client_return_code"] = peer_results[1]["return_code"]
        result["server_csv_path"] = str(server_csv or "")
        result["client_csv_path"] = str(client_csv or "")
        result["proxy_stats"] = edge_stats[0]["stats"] if edge_stats else {}
        result["metrics"] = combined_summary(
            server_csv, client_csv, assessment_elapsed_ms=effective_stream_ms,
        )
        if "peer_profiles" in scenario:
            local_contracts = []
            for peer_profile, peer_result in zip(peer_profiles, peer_results):
                actual = peer_result["csv_summary"]
                expected = {
                    "playback_prefill_frames": peer_profile.playback_prefill_frames,
                    "playback_max_frames": peer_profile.playback_max_frames,
                    "capture_ring_frames": peer_profile.capture_ring_frames,
                    "playback_ring_frames": peer_profile.playback_ring_frames,
                    "playout_delay_frames": peer_profile.playout_delay_frames,
                    "jitter_buffer_target_frames": peer_profile.jitter_buffer_frames,
                    "jitter_buffer_max_frames": peer_profile.jitter_buffer_max_frames,
                }
                observed = {name: int(actual.get(name, 0)) for name in expected}
                local_contracts.append({
                    "profile": peer_profile.base_profile,
                    "expected": expected,
                    "observed": observed,
                    "ok": observed == expected,
                })
            result["local_profile_contracts"] = local_contracts
            result["asymmetric_profile_contract_valid"] = (
                all(item["ok"] for item in local_contracts)
                and all(
                    int(peer["csv_summary"].get("session_frames_per_packet", 0)) ==
                        int(peer_profiles[0].frame_size)
                    for peer in peer_results
                )
            )
        server_paths = {"dir": str(case_root / "peer-1")}; client_paths = {"dir": str(case_root / "peer-2")}
        wav = analyze_metronome_recordings(result, server_paths, client_paths)
        if wav: result["metronome_wav_analysis"] = wav
        pulse = analyze_listener_compensated_pulse(result, server_paths, client_paths)
        if pulse: result["listener_compensated_pulse_analysis"] = pulse
        epoch = analyze_metro_pulse_epoch(result, server_paths, client_paths)
        if epoch: result["metro_pulse_epoch_analysis"] = epoch
        result["verdict"] = verdict_for(result)
    else:
        result["mesh_metrics"] = mesh_collect_metrics(peer_results, effective_stream_ms)
        result["verdict"] = mesh_verdict(result)
    (case_root / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def run(args: Any, repo: Path, invocation: InvocationArtifacts,
        manifest: InvocationManifest) -> int:
    try:
        if args.stream_ms <= 0:
            raise ValueError("--stream-ms must be positive")
        if args.startup_timeout_s <= 0:
            raise ValueError("--startup-timeout-s must be positive")
        if args.scenario_cooldown_s < 0:
            raise ValueError("--scenario-cooldown-s cannot be negative")
        if args.headless_audio_buffer_frames <= 0:
            raise ValueError("--headless-audio-buffer-frames must be positive")
        devices = (args.server_audio_device, args.client_audio_device)
        if args.mesh_peers and any(device is not None for device in devices):
            raise ValueError("mesh stress retains deterministic headless audio; omit physical device options")
        capabilities = NativeCapabilities(args.jam2)
        configure_native_profiles(capabilities.description)
        from .scenarios import mesh_peer_counts, mesh_scenario_plan, scenario_plan
        if args.mesh_peers:
            counts = mesh_peer_counts(args)
            catalog, ids = mesh_scenario_plan(args.profile, args.scenario, counts)
        else:
            catalog, ids = scenario_plan(args.profile, args.scenario, args.include_audio_probes)
        priorities = args.os_priority or ["high"]
        if "all" in priorities:
            priorities = ["off", "high", "realtime"]
        priorities = list(dict.fromkeys(priorities))
        formats = ["pcm16", "pcm24"] if args.network_audio_format == "both" else [args.network_audio_format]
        plan = []
        for case_id in ids:
            for priority in priorities:
                base_id = case_id if len(priorities) == 1 else f"{case_id}__os_{priority}"
                for audio_format in formats:
                    planned_id = base_id if args.network_audio_format == "pcm24" else f"{base_id}__{audio_format}"
                    selected = dict(catalog.get(case_id, {}))
                    profile = selected.get("profile")
                    if profile is not None:
                        capabilities.validate_sparse_overrides(profile.overrides)
                    for peer_profile in selected.get("peer_profiles", []):
                        capabilities.validate_sparse_overrides(peer_profile.overrides)
                    selected.setdefault("source_scenario", case_id)
                    selected["os_priority"] = priority
                    selected["network_audio_format"] = audio_format
                    if args.network_audio_format != "pcm24":
                        selected.setdefault("signal", "tone-440")
                    plan.append((planned_id, case_id, selected))
        results = []
        for index, (planned_id, case_id, selected) in enumerate(plan):
            if case_id not in catalog: raise ValueError(f"unknown stress scenario: {case_id}")
            case_root = invocation.root / "cases" / normalized_path_id(planned_id)
            case_root.mkdir(parents=True)
            result = _run_case(args.jam2, planned_id, selected, args, case_root, args.seed + index)
            results.append(result)
            status = "passed" if result.get("verdict") in ("pass", "expected_impairment") else "failed"
            manifest.add_case({"id": planned_id, "source_case": case_id,
                               "status": status, "verdict": result.get("verdict"),
                               "result": str((case_root / "result.json").relative_to(invocation.root))})
            if index + 1 < len(plan) and args.scenario_cooldown_s:
                time.sleep(args.scenario_cooldown_s)
        write_results_csv(invocation.root / "results.csv", results)
        passed = all(case["status"] == "passed" for case in manifest.data["cases"])
        if args.network_audio_format == "both":
            comparison = write_format_comparison(invocation.root, results)
            if passed and comparison.get("pair_count") != len(plan) // 2:
                raise RuntimeError(
                    "matched PCM16/PCM24 stress results did not produce every comparison pair")
        manifest.finish("passed" if passed else "failed", 0 if passed else 1,
                        profiles={name: capabilities.profile(name) for name in capabilities.profiles})
        return 0 if passed else 1
    except Exception as error:
        manifest.add_case({"id": "stress-infrastructure", "status": "infrastructure-error", "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
