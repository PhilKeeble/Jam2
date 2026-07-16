from __future__ import annotations

import csv
import json
import hashlib
import hmac
import os
import re
import secrets
import socket
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path
from typing import Any

from .artifacts import InvocationArtifacts
from .audio_analysis import (
    analyze_metronome_wav,
    analyze_recording_dir,
    basic_stats,
    detect_transients,
    estimate_tone,
    read_wav_mono,
)
from .manifest import InvocationManifest
from .metrics import summarize_csv
from .native import NativeCapabilities, ReactiveProcess, native_manifest, run_logged, write_scenario
from .results import (
    METRONOME_WAV_TOLERANCE_FRAMES,
    analyze_listener_compensated_pulse_side,
    analyze_metro_pulse_epoch_side,
    listener_compensation_contract,
    mesh_collect_metrics,
    mesh_verdict,
)


ACTION_CASES = {
    "metronome.enabled": "grid-stop-restart-shared-grid",
    "metronome.bpm": "grid-authority-client-shared-grid",
    "metronome.mode": "grid-authority-client-leader-audio",
    "metronome.level": "runtime-controls",
    "remote.level": "runtime-controls",
    "track.sync": "transport-track-sync-off",
    "track.load": "transport-track-actions",
    "track.play": "transport-track-actions",
    "track.stop": "transport-track-actions",
    "track.restart": "transport-track-actions",
    "track.record-start": "transport-record-start-joiner",
    "recording.start": "mesh-3-clean",
    "recording.stop": "mesh-3-clean",
    "snapshot": "runtime-controls",
    "shutdown": "clean-control",
}


class ValidationReporter:
    def __init__(self, total: int, artifact_root: Path) -> None:
        self.total = total
        self.artifact_root = artifact_root
        self.started_at = time.monotonic()
        self.index = 0
        self.passed = 0
        self.active_name: str | None = None
        self.active_started_at = 0.0

    def start(self, name: str) -> None:
        if self.active_name is not None:
            raise RuntimeError(f"validation reporter still has an active case: {self.active_name}")
        self.index += 1
        self.active_name = name
        self.active_started_at = time.monotonic()
        print(f"[RUN ] {self.index:02d}/{self.total:02d} {name}", flush=True)

    def finish(self, name: str, passed: bool, summary: str = "", failure_detail: str = "") -> None:
        if self.active_name != name:
            raise RuntimeError(
                f"validation reporter expected {self.active_name!r}, received {name!r}")
        elapsed = time.monotonic() - self.active_started_at
        if passed:
            self.passed += 1
        label = "PASS" if passed else "FAIL"
        detail = summary if passed else (failure_detail or summary)
        suffix = f" {detail}" if detail else ""
        print(
            f"[{label}] {self.index:02d}/{self.total:02d} {name} ({elapsed:.1f}s){suffix}",
            flush=True,
        )
        self.active_name = None

    def fail_setup(self, error: str) -> None:
        if self.active_name is not None:
            self.finish(
                self.active_name, False,
                failure_detail=f"reason={error} artifacts={self.artifact_root}",
            )
        else:
            print(f"[FAIL] setup reason={error} artifacts={self.artifact_root}", flush=True)

    def summary(self, passed: bool, infrastructure_error: str | None = None) -> None:
        elapsed = time.monotonic() - self.started_at
        if infrastructure_error is not None:
            print(
                f"[SUMMARY] INFRASTRUCTURE-ERROR artifacts={self.artifact_root}",
                flush=True,
            )
            return
        label = "PASS" if passed else "FAIL"
        print(
            f"[SUMMARY] {label} {self.passed}/{self.total} ({elapsed:.1f}s) "
            f"artifacts={self.artifact_root}",
            flush=True,
        )


def _case(
    manifest: InvocationManifest,
    reporter: ValidationReporter,
    name: str,
    status: str,
    *,
    console_summary: str = "",
    failure_detail: str = "",
    **evidence: Any,
) -> bool:
    manifest.add_case({"id": name, "status": status, **evidence})
    passed = status == "passed"
    reporter.finish(
        name,
        passed,
        console_summary,
        failure_detail or f"artifacts={reporter.artifact_root / name}",
    )
    return passed


def _reserve_dual_port() -> int:
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
            tcp.close()
            udp.close()
    raise RuntimeError("could not reserve a localhost TCP/UDP port pair")


def _base_scenario(run_id: str, operation: str, root: Path) -> dict[str, Any]:
    return {
        "schema": "jam2-debug-scenario",
        "run_id": run_id,
        "operation": operation,
        "profile": "fast",
        "runtime": {
            "headless_audio": True,
            "audio_buffer_size": 256,
            "sample_rate": 48000,
            "stats": True,
            "stats_interval_ms": 250,
            "stats_warmup_ms": 0,
        },
        "artifacts": {"root": str(root)},
    }


def _wav_boundary_fixtures(root: Path) -> list[str]:
    fixture_root = root / "generated-corpus"
    fixture_root.mkdir()

    def write_pcm16(path: Path, sample_rate: int) -> None:
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(sample_rate)
            output.writeframes(struct.pack("<" + "h" * 128, *([0] * 128)))

    matching = fixture_root / "pcm16-48000.wav"
    mismatch = fixture_root / "pcm16-44100.wav"
    write_pcm16(matching, 48000)
    write_pcm16(mismatch, 44100)
    truncated = fixture_root / "truncated-riff.wav"
    truncated.write_bytes(b"RIFF\x10\x00\x00\x00WAVEfmt ")
    excessive = fixture_root / "excessive-data.wav"
    excessive.write_bytes(
        b"RIFF" + struct.pack("<I", 36) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16) +
        b"data" + struct.pack("<I", 0x7FFFFFF0)
    )
    return [
        f"valid:{matching}",
        f"valid:{mismatch}",
        f"invalid:{truncated}",
        f"invalid:{excessive}",
        f"import-match-48000:{matching}",
        f"import-mismatch-48000:{mismatch}",
    ]


def _run_framework(repo: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
                   reporter: ValidationReporter) -> bool:
    reporter.start("framework-self-tests")
    case_root = invocation.root / "framework"
    case_root.mkdir()
    command = [sys.executable, "-m", "unittest", "discover", "-s", str(repo / "tools"), "-p", "test_*.py"]
    result = run_logged(command, case_root / "stdout.log", case_root / "stderr.log", timeout=120)
    return _case(manifest, reporter, "framework-self-tests", "passed" if result["return_code"] == 0 else "failed", process=result)


def _run_help_contract(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
                       reporter: ValidationReporter) -> bool:
    reporter.start("public-command-surface")
    case_root = invocation.root / "public-surface"
    case_root.mkdir()
    commands = [
        [str(jam2), "-h"], [str(jam2), "local", "-h"],
        [str(jam2), "network", "-h"], [str(jam2), "network", "create", "-h"],
        [str(jam2), "network", "join", "-h"], [str(jam2), "debug", "-h"],
        [str(jam2), "debug", "run", "-h"], [str(jam2), "debug", "describe", "-h"],
    ]
    results = []
    ok = True
    for index, command in enumerate(commands):
        result = run_logged(command, case_root / f"{index:02d}.stdout.log", case_root / f"{index:02d}.stderr.log", timeout=20)
        results.append(result)
        ok &= result["return_code"] == 0
    removed_commands = [
        [str(jam2), "listen", "-h"],
        [str(jam2), "connect", "-h"],
        [str(jam2), "mesh", "-h"],
        [str(jam2), "network", "listen", "-h"],
        [str(jam2), "network", "connect", "-h"],
        [str(jam2), "network", "mesh", "-h"],
    ]
    removed_results = []
    for index, command in enumerate(removed_commands):
        result = run_logged(
            command, case_root / f"removed-{index:02d}.stdout.log",
            case_root / f"removed-{index:02d}.stderr.log", timeout=20,
        )
        removed_results.append(result)
        ok &= result["return_code"] != 0
    return _case(
        manifest, reporter, "public-command-surface", "passed" if ok else "failed",
        processes=results, removed_commands=removed_results)


def _run_focused(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
                 reporter: ValidationReporter) -> bool:
    ok = True
    for operation in ("validate.boundaries", "validate.controller-lifecycle"):
        name = operation.replace(".", "-")
        reporter.start(name)
        root = invocation.root / name
        root.mkdir()
        scenario = _base_scenario(name, operation, root)
        scenario.pop("runtime")
        if operation == "validate.boundaries":
            scenario["fixtures"] = _wav_boundary_fixtures(root)
        elif operation == "validate.controller-lifecycle":
            scenario["network"] = {
                "heartbeat_interval_ms": 20,
                "heartbeat_miss_limit": 3,
            }
        scenario_path = root / "scenario.json"
        write_scenario(scenario_path, scenario)
        result = run_logged(
            [str(jam2), "debug", "run", str(scenario_path)],
            root / "stdout.log", root / "stderr.log", timeout=60,
        )
        try:
            native = native_manifest(root)
            passed = result["return_code"] == 0 and native.get("ok") is True
        except Exception as error:
            native = {"error": str(error)}
            passed = False
        ok &= _case(manifest, reporter, name, "passed" if passed else "failed", process=result, native=native)
    return ok


def _run_schema_parity(repo: Path, capabilities: NativeCapabilities,
                       manifest: InvocationManifest, reporter: ValidationReporter) -> bool:
    reporter.start("description-schema-operation-parity")
    schema_path = repo / "tools" / "scenarios" / "jam2-debug-scenario.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    schema_operations = schema["properties"]["operation"]["enum"]
    described_operations = capabilities.description["operations"]
    schema_runtime_fields = set(schema["properties"]["runtime"]["properties"])
    described_runtime_fields = {item["name"] for item in capabilities.description["runtime_fields"]}
    schema_actions = schema["properties"]["actions"]["items"]["properties"]["type"]["enum"]
    passed = (schema.get("$id") == "jam2-debug-scenario" and
              schema["properties"]["schema"].get("const") == "jam2-debug-scenario" and
              schema_operations == described_operations and
              schema_runtime_fields == described_runtime_fields and
              schema_actions == capabilities.description["actions"] and
              schema["properties"]["actions"]["maxItems"] == capabilities.description["max_actions"] and
              set(ACTION_CASES) == set(capabilities.description["actions"]))
    return _case(
        manifest, reporter, "description-schema-operation-parity",
        "passed" if passed else "failed",
        schema_operations=schema_operations,
        described_operations=described_operations,
        runtime_field_count=len(schema_runtime_fields),
        action_count=len(schema_actions),
    )


def _run_local(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
               reporter: ValidationReporter) -> bool:
    reporter.start("local-effective-configuration")
    root = invocation.root / "local-effective-configuration"
    root.mkdir()
    scenario = _base_scenario("local-effective-configuration", "local", root)
    scenario["runtime"].update({"stream_ms": 800, "bpm": 137, "test_input": "tone-440"})
    scenario_path = root / "scenario.json"
    write_scenario(scenario_path, scenario)
    result = run_logged([str(jam2), "debug", "run", str(scenario_path)], root / "stdout.log", root / "stderr.log", timeout=30)
    try:
        native = native_manifest(root)
        effective = native["effective_configuration"]
        artifacts = native["artifacts"]
        passed = (result["return_code"] == 0 and effective["sample_rate"] == 48000 and
                  effective["audio_buffer_size"] == 256 and native.get("ok") is True and
                  any(item["path"] == "scenario.json" for item in artifacts))
    except Exception as error:
        native = {"error": str(error)}
        passed = False
    return _case(manifest, reporter, "local-effective-configuration", "passed" if passed else "failed", process=result, native=native)


def _run_real_device(jam2: Path, invocation: InvocationArtifacts,
                     manifest: InvocationManifest, device_text: str,
                     reporter: ValidationReporter) -> bool:
    reporter.start("real-device-extension")
    root = invocation.root / "real-device-extension"
    root.mkdir()
    try:
        device_id = int(device_text)
    except ValueError as error:
        raise ValueError("--real-device must be a numeric device identifier") from error
    if not 0 <= device_id <= 65535:
        raise ValueError("--real-device is outside the native device identifier bound")
    scenario = _base_scenario("real-device-extension", "local", root)
    scenario["runtime"].pop("headless_audio")
    scenario["runtime"].update({
        "audio_device": device_id,
        "stream_ms": 1200,
        "test_input": "tone-440",
    })
    path = root / "scenario.json"
    write_scenario(path, scenario)
    result = run_logged([str(jam2), "debug", "run", str(path)],
                        root / "stdout.log", root / "stderr.log", timeout=30)
    try:
        native = native_manifest(root)
        passed = (result["return_code"] == 0 and native.get("ok") is True and
                  native.get("effective_configuration", {}).get("audio_device") == device_id)
    except Exception as error:
        native = {"error": str(error)}
        passed = False
    return _case(manifest, reporter, "real-device-extension", "passed" if passed else "failed",
                 process=result, native=native,
                 coverage="explicit physical-device extension; headless baseline also ran")


def _run_invalid_contracts(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
                           reporter: ValidationReporter) -> bool:
    reporter.start("schema-and-numeric-rejection")
    root = invocation.root / "invalid-contracts"
    root.mkdir()
    cases = [
        ("obsolete-v1", {"schema": "jam2-debug-scenario-v1", "run_id": "obsolete-v1", "operation": "local", "artifacts": {"root": str(root / "v1")}}),
        ("frame-size-low", {**_base_scenario("frame-size-low", "local", root / "low"), "runtime": {"headless_audio": True, "frame_size": 31}}),
        ("unknown-field", {**_base_scenario("unknown-field", "local", root / "unknown"), "runtime": {"headless_audio": True, "not_native": 1}}),
        ("wrong-container", {**_base_scenario("wrong-container", "local", root / "container"), "network": "not-an-object"}),
        ("wrong-network-integer", {**_base_scenario("wrong-network-integer", "local", root / "integer"), "network": {"wait_ms": "not-an-integer"}}),
        ("wrong-action-field", {**_base_scenario("wrong-action-field", "network.create", root / "action"), "actions": [{"type": "metronome.bpm", "value": "120"}]}),
    ]
    results = []
    ok = True
    for name, scenario in cases:
        path = root / f"{name}.json"
        path.write_text(json.dumps(scenario), encoding="utf-8")
        result = run_logged([str(jam2), "debug", "run", str(path)], root / f"{name}.stdout.log", root / f"{name}.stderr.log", timeout=20)
        results.append(result)
        ok &= result["return_code"] != 0
    return _case(manifest, reporter, "schema-and-numeric-rejection", "passed" if ok else "failed", processes=results)


def _recv_exact(connection: socket.socket, size: int) -> bytes:
    output = bytearray()
    while len(output) < size:
        block = connection.recv(size - len(output))
        if not block:
            raise RuntimeError("control socket closed while receiving a frame")
        output.extend(block)
    return bytes(output)


def _recv_control_frame(connection: socket.socket) -> bytes:
    size = struct.unpack(">I", _recv_exact(connection, 4))[0]
    if size < 1 or size > 65564:
        raise RuntimeError(f"control frame length is outside the native bound: {size}")
    return _recv_exact(connection, size)


def _send_fragmented(connection: socket.socket, payload: bytes) -> None:
    framed = struct.pack(">I", len(payload)) + payload
    offsets = (1, 2, 3, 5, 8, 13)
    position = 0
    index = 0
    while position < len(framed):
        amount = offsets[index % len(offsets)]
        connection.sendall(framed[position:position + amount])
        position += amount
        index += 1


def _protocol_field(value: bytes) -> bytes:
    return struct.pack(">I", len(value)) + value


def _keyed_value(key: bytes, domain: bytes, transcript: bytes) -> bytes:
    return hmac.new(key, _protocol_field(domain) + transcript, hashlib.sha256).digest()


def _authenticated_body(message: dict[str, Any], key: bytes, sequence: int) -> bytes:
    payload = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    body = b"\x02\x01\x00\x00" + struct.pack(">Q", sequence) + (b"\x00" * 16) + payload
    tag = hmac.new(key, body, hashlib.sha256).digest()[:16]
    return body[:12] + tag + body[28:]


def _authenticate_fragmented(
    connection: socket.socket,
    session_hex: str,
    key_hex: str,
    peer_token: str,
    udp_endpoint: str,
) -> bytes:
    challenge = json.loads(_recv_control_frame(connection).decode("utf-8"))
    server_nonce = bytes.fromhex(challenge["server_nonce"])
    client_nonce = bytes.fromhex("10" * 16)
    transcript = (
        b"jam2-control-v2" +
        _protocol_field(session_hex.encode("ascii")) +
        _protocol_field(server_nonce) +
        _protocol_field(client_nonce) +
        _protocol_field(peer_token.encode("utf-8")) +
        _protocol_field(udp_endpoint.encode("utf-8"))
    )
    master_key = bytes.fromhex(key_hex)
    proof = _keyed_value(master_key, b"jam2-control-client-proof", transcript)[:16]
    hello = {
        "type": "hello.proof", "version": 2, "session": session_hex,
        "client_nonce": client_nonce.hex(), "peer_token": peer_token,
        "udp_endpoint": udp_endpoint, "proof": proof.hex(),
    }
    _send_fragmented(
        connection,
        json.dumps(hello, separators=(",", ":")).encode("utf-8"),
    )
    response = json.loads(_recv_control_frame(connection).decode("utf-8"))
    expected_server_proof = _keyed_value(
        master_key, b"jam2-control-server-proof", transcript)[:16].hex()
    if response.get("type") != "hello.ok" or response.get("proof") != expected_server_proof:
        raise RuntimeError("fragmented control authentication did not produce a valid server proof")
    return _keyed_value(master_key, b"jam2-control-c2s", transcript)


def _run_real_process_control_hardening(
    jam2: Path,
    invocation: InvocationArtifacts,
    manifest: InvocationManifest,
    reporter: ValidationReporter,
) -> bool:
    reporter.start("real-process-control-hardening")
    root = invocation.root / "real-process-control-hardening"
    root.mkdir()
    port = _reserve_dual_port()
    session_hex = "1020304050607080"
    key_hex = "00112233445566778899aabbccddeeff"
    creator_token = "00000000000000010000000000000001"
    peer_token = "00000000000000020000000000000002"
    scenario = _base_scenario("real-process-control-hardening", "network.create", root)
    scenario["runtime"].update({"stream_ms": 0, "test_input": "silence"})
    scenario["network"] = {
        "bind": f"127.0.0.1:{port}", "no_stun": True,
        "session_id": session_hex, "session_key": key_hex,
        "peer_token": creator_token, "max_peers": 4,
    }
    scenario["actions"] = [{
        "id": "bounded-hardening-shutdown", "type": "shutdown",
        "after_event": "controller_started", "delay_frames": 144000,
    }]
    scenario_path = root / "scenario.json"
    write_scenario(scenario_path, scenario)
    valid_section = {
        "label": "A", "name": "Verse", "beats": 4,
        "chords": [], "beat_notes": [], "lyrics": [], "beat_patterns": [],
    }
    mismatched_banks = [
        {"id": chr(ord("A") + index), "lanes": []} for index in range(4)
    ]
    mismatched_banks[0]["lanes"].append({
        "id": "wrong-rate-lane", "asset_hash": "b" * 64,
        "name": "Wrong Rate WAV", "sample_rate": 44100,
        "start_frame": "0", "stop_frame": "-1",
        "loop_start_frame": "-1", "loop_end_frame": "-1",
        "gain_db": 0.0, "muted": False, "solo": False,
        "loop_enabled": True,
    })
    corpus = [
        {
            "type": "session.membership", "revision": 1,
            "page_index": 0, "page_count": 1,
            "coordinator_token": creator_token, "peers": [],
        },
        {"type": "metronome.settings", "bpm": 126},
        {
            "type": "looper.recording.offer",
            "recording_id": "12345678-1234-1234-1234-123456789abc",
            "bank": 0, "target_lane_id": "peer-lane",
            "sha256": "a" * 64, "name": "wrong-rate", "sample_rate": 44100,
        },
        {"type": "looper.asset.request", "hashes": ["a" * 64] * 65},
        {
            "type": "song.set", "arrangement_revision": 1,
            "song": {
                "format": "jam2.song.v1", "title": "wrong-rate asset",
                "lyrics_text": "", "sections": [valid_section],
                "looper": {
                    "format": "jam2.looper.v1", "active_bank": 0,
                    "grid_lock": True, "banks": mismatched_banks,
                },
            },
        },
        {
            "type": "song.set", "arrangement_revision": 1,
            "song": {
                "format": "jam2.song.v1", "title": "hostile nested model",
                "lyrics_text": "", "sections": [{
                    "label": "A", "name": "Verse", "beats": 513,
                    "chords": [], "beat_notes": [], "lyrics": [],
                    "beat_patterns": [],
                }],
            },
        },
    ]
    (root / "generated-control-corpus.json").write_text(
        json.dumps(corpus, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    stdout = (root / "stdout.log").open("w", encoding="utf-8", newline="")
    stderr = (root / "stderr.log").open("w", encoding="utf-8", newline="")
    process = subprocess.Popen(
        [str(jam2), "debug", "run", str(scenario_path)],
        stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, text=True,
    )
    connection: socket.socket | None = None
    pending: list[socket.socket] = []
    protocol_error = ""
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
                break
            except OSError:
                if process.poll() is not None:
                    break
                time.sleep(0.02)
        if connection is None:
            raise RuntimeError("native coordinator did not open its TCP control listener")
        connection.settimeout(2)
        c2s_key = _authenticate_fragmented(
            connection, session_hex, key_hex, peer_token, "127.0.0.1:41002")
        sequence = 1
        for message in corpus:
            _send_fragmented(connection, _authenticated_body(message, c2s_key, sequence))
            sequence += 1
        for index in range(48):
            _send_fragmented(
                connection,
                _authenticated_body(
                    {"type": "unsupported.flood", "index": index}, c2s_key, sequence),
            )
            sequence += 1
        for _ in range(12):
            try:
                candidate = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                candidate.settimeout(0.2)
                pending.append(candidate)
            except OSError:
                pass
        time.sleep(0.25)
        invalid_tag = bytearray(_authenticated_body(
            {"type": "session.heartbeat.ack", "sequence": 1}, c2s_key, sequence))
        invalid_tag[12] ^= 0x01
        _send_fragmented(connection, bytes(invalid_tag))
        time.sleep(0.1)
    except Exception as error:
        protocol_error = f"{type(error).__name__}: {error}"
    finally:
        if connection is not None:
            connection.close()
        for candidate in pending:
            candidate.close()
        code, timed_out = _wait_process(process, 12)
        stdout.close()
        stderr.close()

    try:
        native = native_manifest(root)
        result = native.get("result", {})
        server = result.get("control_server", {})
        passed = (
            not protocol_error and code == 0 and not timed_out and native.get("ok") is True and
            result.get("control_authorization_rejections", 0) >= 1 and
            result.get("control_validation_rejections", 0) >= 53 and
            server.get("pending_cap_rejects", 0) >= 1 and
            server.get("tag_or_sequence_rejects", 0) >= 1 and
            server.get("input_high_water_bytes", 0) <= 65568
        )
    except Exception as error:
        native = {"error": str(error)}
        passed = False
    return _case(
        manifest, reporter, "real-process-control-hardening", "passed" if passed else "failed",
        process={"return_code": code, "timed_out": timed_out},
        protocol_error=protocol_error, pending_connections=len(pending), native=native)


def _run_reactive_channel(jam2: Path, invocation: InvocationArtifacts,
                          manifest: InvocationManifest, reporter: ValidationReporter) -> bool:
    reporter.start("reactive-inherited-channel")
    root = invocation.root / "reactive-channel"
    root.mkdir()
    port = _reserve_dual_port()
    scenario = _base_scenario("reactive-channel", "network.create", root)
    scenario["runtime"]["stream_ms"] = 900
    scenario["network"] = {
        "bind": f"127.0.0.1:{port}", "no_stun": True,
        "session_id": f"{secrets.randbits(64) or 1:016x}",
        "session_key": secrets.token_hex(16), "peer_token": secrets.token_hex(16),
    }
    scenario["automation"] = {"reactive": True, "controller_loss": "stop"}
    path = root / "scenario.json"; write_scenario(path, scenario)
    process = ReactiveProcess.start(jam2, path, root / "stdout.log", root / "stderr.log")
    deadline = time.monotonic() + 5
    sent = False
    invalid_sent = False
    shutdown_sent = False
    while time.monotonic() < deadline and process.process.poll() is None:
        if not sent and any(event.get("event") == "hello" for event in process.events):
            process.send({"type": "action", "action": {"type": "metronome.bpm", "value": "invalid"}})
            invalid_sent = True
            process.send({"type": "snapshot", "id": "reactive-snapshot"})
            sent = True
        if sent and not shutdown_sent and any(event.get("event") == "snapshot" and event.get("id") == "reactive-snapshot" for event in process.events):
            process.send({"type": "shutdown", "id": "reactive-shutdown"})
            shutdown_sent = True
        time.sleep(0.01)
    code = process.wait(10)
    events = list(process.events)
    try:
        native = native_manifest(root)
        names = {event.get("event") for event in events}
        passed = (code == 0 and sent and invalid_sent and shutdown_sent and
                  {"hello", "command_rejected", "snapshot", "shutdown"}.issubset(names) and
                  native.get("reactive") is True)
    except Exception as error:
        native = {"error": str(error)}; passed = False
    (root / "events.json").write_text(json.dumps(events, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    loss_results = []
    for policy in ("stop", "continue"):
        loss_root = invocation.root / f"reactive-controller-loss-{policy}"
        loss_root.mkdir()
        loss_port = _reserve_dual_port()
        loss_scenario = _base_scenario(
            f"reactive-controller-loss-{policy}", "network.create", loss_root)
        loss_scenario["runtime"]["stream_ms"] = 1200
        loss_scenario["network"] = {
            "bind": f"127.0.0.1:{loss_port}", "no_stun": True,
            "session_id": f"{secrets.randbits(64) or 1:016x}",
            "session_key": secrets.token_hex(16),
            "peer_token": secrets.token_hex(16),
        }
        loss_scenario["automation"] = {
            "reactive": True, "controller_loss": policy,
        }
        if policy == "continue":
            loss_scenario["actions"] = [{
                "id": "continued-shutdown", "type": "shutdown",
                "after_event": "controller_started", "delay_frames": 24000,
            }]
        loss_path = loss_root / "scenario.json"
        write_scenario(loss_path, loss_scenario)
        loss_process = ReactiveProcess.start(
            jam2, loss_path, loss_root / "stdout.log", loss_root / "stderr.log")
        hello_deadline = time.monotonic() + 5
        while (time.monotonic() < hello_deadline and
               loss_process.process.poll() is None and
               not any(event.get("event") == "hello" for event in loss_process.events)):
            time.sleep(0.01)
        saw_hello = any(event.get("event") == "hello" for event in loss_process.events)
        loss_process.command_writer.close()
        loss_code = loss_process.wait(10)
        try:
            loss_native = native_manifest(loss_root)
            final_frame = int(loss_native.get("result", {}).get("final_engine_frame", 0))
            policy_result = loss_native.get("result", {})
            stopped_as_error = (
                policy == "stop" and loss_code == 5 and
                loss_native.get("ok") is False and final_frame < 24000)
            continued_to_shutdown = (
                policy == "continue" and loss_code == 0 and
                loss_native.get("ok") is True and final_frame >= 24000)
            loss_ok = (saw_hello and
                       policy_result.get("controller_disconnects") == 1 and
                       (stopped_as_error or continued_to_shutdown))
        except Exception as error:
            loss_native = {"error": str(error)}
            loss_ok = False
        passed &= loss_ok
        loss_results.append({
            "policy": policy, "passed": loss_ok, "return_code": loss_code,
            "python_event_drops": loss_process.event_drops, "native": loss_native,
        })
    return _case(manifest, reporter, "reactive-inherited-channel", "passed" if passed else "failed",
                 return_code=code, events=[event.get("event") for event in events],
                 python_event_drops=process.event_drops, native=native,
                 controller_loss_policies=loss_results)


def _run_handle_isolation(jam2: Path, invocation: InvocationArtifacts,
                          manifest: InvocationManifest, reporter: ValidationReporter) -> bool:
    reporter.start("automation-handle-isolation")
    root = invocation.root / "automation-handle-isolation"
    root.mkdir()
    static_root = root / "static"
    static_root.mkdir()
    scenario = _base_scenario("static-handle-rejection", "local", static_root)
    scenario["runtime"]["stream_ms"] = 10
    scenario_path = static_root / "scenario.json"
    write_scenario(scenario_path, scenario)
    environment = os.environ.copy()
    environment.update({
        "JAM2_AUTOMATION_COMMAND_HANDLE": "123456",
        "JAM2_AUTOMATION_EVENT_HANDLE": "123457",
    })
    commands = [
        [str(jam2)],
        [str(jam2), "local", "--headless-audio", "on", "--stream-ms", "10"],
        [str(jam2), "debug", "describe", "--json"],
        [str(jam2), "debug", "run", str(scenario_path)],
    ]
    results = [
        run_logged(command, root / f"{index:02d}.stdout.log",
                   root / f"{index:02d}.stderr.log", timeout=20, env=environment)
        for index, command in enumerate(commands)
    ]
    passed = all(result["return_code"] != 0 and not result["timed_out"] for result in results)
    return _case(manifest, reporter, "automation-handle-isolation", "passed" if passed else "failed",
                 processes=results)


def _wait_process(process: subprocess.Popen[str], timeout: float) -> tuple[int, bool]:
    try:
        return process.wait(timeout=timeout), False
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        return process.returncode, True


def _survivor_audio_continued(peer_root: Path) -> bool:
    csv_files = sorted((peer_root / "csv").glob("*.csv"))
    if len(csv_files) != 1:
        return False
    with csv_files[0].open("r", encoding="utf-8", newline="") as handle:
        rows = [
            row for row in csv.DictReader(handle)
            if row.get("row_type") == "periodic" and
            1600.0 <= float(row.get("elapsed_ms", 0.0)) <= 2600.0
        ]
    if len(rows) < 2:
        return False
    return (
        int(float(rows[0].get("network_active_peer_count", 0))) >= 1 and
        int(float(rows[-1].get("network_active_peer_count", 0))) >= 1 and
        float(rows[-1].get("recv_packets", 0.0)) >
            float(rows[0].get("recv_packets", 0.0)) and
        float(rows[-1].get("mix_output_frames", 0.0)) >
            float(rows[0].get("mix_output_frames", 0.0))
    )


def _run_mesh(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest,
              peers: int, reporter: ValidationReporter) -> bool:
    case_id = f"clean-mesh-{peers}-peers"
    reporter.start(case_id)
    root = invocation.root / case_id
    root.mkdir()
    port = _reserve_dual_port()
    session_id = f"{secrets.randbits(64) or 1:016x}"
    session_key = secrets.token_hex(16)
    invite = f"jam2://v1?endpoint=127.0.0.1:{port}&session={session_id}&key={session_key}"
    scenarios: list[tuple[Path, Path]] = []
    for index in range(peers):
        peer_root = root / f"peer-{index}"
        peer_root.mkdir()
        operation = "network.create" if index == 0 else "network.join"
        scenario = _base_scenario(f"{case_id}-peer-{index}", operation, peer_root)
        scenario["runtime"].update({"stream_ms": 2800, "test_input": "pulse-1s" if index == 0 else "silence"})
        scenario["network"] = {
            "bind": f"127.0.0.1:{port}" if index == 0 else "127.0.0.1:0",
            "no_stun": True,
            "wait_ms": 6000,
            "peer_token": secrets.token_hex(16),
        }
        if index == 0:
            scenario["network"].update({"session_id": session_id, "session_key": session_key, "max_peers": peers})
        else:
            scenario["network"]["join_url"] = invite
        if peers == 2:
            scenario["actions"] = [
                {"id": f"metro-{index}", "type": "metronome.enabled", "enabled": True, "after_event": "network.connected", "delay_frames": 4800},
                {"id": f"bpm-{index}", "type": "metronome.bpm", "value": 133, "after_event": "network.connected", "delay_frames": 9600},
                ({"id": f"snapshot-{index}", "type": "snapshot", "apply_frame": 14400}
                 if index == 0 else
                {"id": f"snapshot-{index}", "type": "snapshot", "after_event": "network.connected", "delay_frames": 14400}),
            ]
        elif peers == 3 and index == 2:
            scenario["actions"] = [{
                "id": "ordinary-peer-leave", "type": "shutdown",
                "after_event": "network.connected", "delay_frames": 48000,
            }]
        path = peer_root / "scenario.json"
        write_scenario(path, scenario)
        scenarios.append((path, peer_root))

    processes: list[tuple[subprocess.Popen[str], Any, Any, Path]] = []
    try:
        for index, (scenario, peer_root) in enumerate(scenarios):
            stdout = (peer_root / "stdout.log").open("w", encoding="utf-8", newline="")
            stderr = (peer_root / "stderr.log").open("w", encoding="utf-8", newline="")
            process = subprocess.Popen([str(jam2), "debug", "run", str(scenario)], stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, text=True)
            processes.append((process, stdout, stderr, peer_root))
            if index == 0:
                time.sleep(0.15)
        outcomes = []
        passed = True
        for process, stdout, stderr, peer_root in processes:
            code, timed_out = _wait_process(process, 15)
            stdout.close()
            stderr.close()
            try:
                native = native_manifest(peer_root)
                remote_count = native.get("result", {}).get("remote_peer_count")
                native_ok = native.get("ok") is True
                active_control_peers = int(native.get("result", {}).get("control_max_active_remote_peers", 0))
            except Exception as error:
                native = {"error": str(error)}
                remote_count = None
                native_ok = False
                active_control_peers = 0
            passed &= code == 0 and not timed_out and native_ok and active_control_peers >= 1
            outcomes.append({"peer": peer_root.name, "return_code": code, "timed_out": timed_out, "remote_peer_count": remote_count, "active_control_peers": active_control_peers, "native": native})
        if peers == 3 and len(outcomes) == 3:
            continuing_frames = [
                int(outcomes[index]["native"].get("result", {}).get("final_engine_frame", 0))
                for index in (0, 1)
            ]
            leaving_frame = int(outcomes[2]["native"].get("result", {}).get("final_engine_frame", 0))
            continued_audio = [
                _survivor_audio_continued(root / f"peer-{index}") for index in (0, 1)
            ]
            for index, continued in enumerate(continued_audio):
                outcomes[index]["audio_continued_after_peer_leave"] = continued
            passed &= (
                min(continuing_frames) > leaving_frame and
                leaving_frame >= 48000 and
                all(continued_audio)
            )
    finally:
        for process, stdout, stderr, _ in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
            if not stdout.closed: stdout.close()
            if not stderr.closed: stderr.close()
    return _case(manifest, reporter, case_id, "passed" if passed else "failed", topology={"peers": peers, "engine": "universal-direct-mesh"}, processes=outcomes)


PUBLIC_NETWORK_CASES = (
    "public-cli-network-clean",
    "public-cli-network-tone",
    "public-cli-metronome-shared-grid",
    "public-cli-metronome-leader-audio",
    "public-cli-metronome-metro-pulse",
)
PUBLIC_NETWORK_STREAM_MS = 10_000


def _reserve_unique_dual_ports(count: int) -> list[int]:
    ports: list[int] = []
    while len(ports) < count:
        port = _reserve_dual_port()
        if port not in ports:
            ports.append(port)
    return ports


def _startup_events(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    events: list[dict[str, Any]] = []
    decoder = json.JSONDecoder()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        offset = 0
        while True:
            start = line.find("{", offset)
            if start < 0:
                break
            try:
                value, consumed = decoder.raw_decode(line[start:])
            except json.JSONDecodeError:
                offset = start + 1
                continue
            if isinstance(value, dict) and value.get("event") == "startup":
                events.append(value)
            offset = start + max(1, consumed)
    return events


def _wait_for_creator_invite(
    process: subprocess.Popen[str], stdout_path: Path, timeout_s: float,
) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for event in _startup_events(stdout_path):
            if event.get("mode") == "create" and event.get("stage") == "waiting":
                invite = str(event.get("connection_url", ""))
                if invite.startswith("jam2://"):
                    return invite
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise RuntimeError("public CLI creator did not emit its waiting invitation")


def _mesh_edge_summaries(path: Path) -> dict[str, dict[str, Any]]:
    edges: dict[str, dict[str, Any]] = {}
    if not path.exists():
        return edges
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("mesh_peer "):
            continue
        fields: dict[str, str] = {}
        for token in line.split()[1:]:
            key, separator, value = token.partition("=")
            if separator:
                fields[key] = value
        peer_id = fields.get("peer_id", "")
        if not peer_id:
            continue
        edge = edges.setdefault(peer_id, {
            "peer_id": peer_id,
            "endpoint": fields.get("endpoint", ""),
            "endpoint_proof_verified": False,
            "sent_packets": 0,
            "recv_packets": 0,
        })
        edge["endpoint_proof_verified"] |= fields.get("endpoint_proof_state") in {
            "active", "verified",
        }
        for name in ("sent_packets", "recv_packets"):
            try:
                edge[name] = max(int(edge[name]), int(fields.get(name, "0")))
            except ValueError:
                pass
    return edges


def _csv_full_mesh_contract(summary: dict[str, Any], expected_remote_peers: int) -> bool:
    """Use structured interval data, which survives orderly peer teardown."""
    return bool(summary.get("has_csv", False)) and (
        int(summary.get("network_peer_count_observed_max", 0)) >= expected_remote_peers and
        int(summary.get("network_active_peer_count_observed_max", 0)) >= expected_remote_peers and
        float(summary.get("sent_packets", 0.0)) > 0.0 and
        float(summary.get("recv_packets", 0.0)) > 0.0 and
        float(summary.get("udp_authentication_failed", 0.0)) == 0.0
    )


def _steady_tone(recording_dir: Path, stem: str) -> dict[str, Any]:
    wav = read_wav_mono(recording_dir / f"{stem}.wav")
    rate = int(wav["sample_rate"])
    samples = wav["samples"]
    start = min(len(samples), rate)
    stop = max(start, len(samples) - rate // 2)
    steady = samples[start:stop]
    stats = basic_stats(steady)
    tone = estimate_tone(steady, rate, 440.0)
    ok = (
        stats.get("rms", 0.0) >= 0.01 and
        stats.get("dropout_runs", 0) == 0 and
        tone.get("tone_present", False) and
        abs(tone.get("error_hz", 999.0)) <= 5.0
    )
    return {"ok": ok, "stem": stem, "steady_stats": stats, "tone": tone}


def _pulse_count(recording_dir: Path, stem: str) -> dict[str, Any]:
    wav = read_wav_mono(recording_dir / f"{stem}.wav")
    rate = int(wav["sample_rate"])
    samples = wav["samples"][min(len(wav["samples"]), rate):]
    frames = detect_transients(
        samples, threshold=0.08, refractory_frames=max(512, rate // 2))
    return {"ok": len(frames) >= 6, "stem": stem, "count": len(frames)}


def _analyze_received_leader_audio(recording_dir: Path) -> dict[str, Any]:
    wav = read_wav_mono(recording_dir / "their-input.wav")
    rate = int(wav["sample_rate"])
    clicks = detect_transients(
        wav["samples"], threshold=0.5, refractory_frames=max(1024, rate // 4))
    expected_interval = rate // 2
    errors = [
        abs((current - previous) - expected_interval)
        for previous, current in zip(clicks, clicks[1:])
    ]
    tone = estimate_tone(wav["samples"], rate, 440.0)
    max_error = max(errors, default=0)
    ok = (
        16 <= len(clicks) <= 24 and
        bool(errors) and max_error <= 240 and
        tone.get("tone_present", False) and
        abs(tone.get("error_hz", 999.0)) <= 5.0
    )
    return {
        "ok": ok,
        "verdict": "pass" if ok else "leader_audio_content_or_click_timing_failed",
        "actual_clicks": len(clicks),
        "expected_interval_frames": expected_interval,
        "max_interval_error_frames": max_error,
        "tone": tone,
        "recording_dir": str(recording_dir),
    }


def _listener_compensation_contract(
    csv_summary: dict[str, Any],
    audio_analysis: dict[str, Any],
    expected_remote_peers: int,
) -> dict[str, Any]:
    return listener_compensation_contract(
        csv_summary, audio_analysis, expected_remote_peers)


def _public_audio_analysis(case_id: str, peer_index: int, recording_dir: Path) -> dict[str, Any]:
    if case_id == "public-cli-network-clean":
        analysis = analyze_recording_dir(recording_dir, signal="silence")
        metro = analyze_metronome_wav(recording_dir, allow_silent=True)
        analysis["metronome"] = metro
        if not metro.get("ok", False):
            analysis["tags"].append(metro.get("verdict", "unexpected_metronome_audio"))
        analysis["ok"] = not analysis["tags"]
        return analysis

    if case_id == "public-cli-network-tone":
        local_signals = ("tone-440", "pulse-1s", "silence", "silence")
        remote_signals = ("pulse-1s", "tone-440", "tone-440", "tone-440")
        analysis = analyze_recording_dir(
            recording_dir,
            signal="mixed-audio",
            local_signal=local_signals[peer_index],
            remote_signal=remote_signals[peer_index],
            ignore_pop_events=True,
        )
        probes = []
        if peer_index == 0:
            probes.extend((_steady_tone(recording_dir, "my-input"),
                           _pulse_count(recording_dir, "their-input")))
        elif peer_index == 1:
            probes.extend((_pulse_count(recording_dir, "my-input"),
                           _steady_tone(recording_dir, "their-input")))
        else:
            probes.append(_steady_tone(recording_dir, "their-input"))
        analysis["validation_probes"] = probes
        for probe in probes:
            if not probe.get("ok", False):
                analysis["tags"].append(f"{probe.get('stem', 'audio')}_steady_probe_failed")
        analysis["ok"] = not analysis["tags"]
        return analysis

    if case_id == "public-cli-metronome-leader-audio":
        analysis = analyze_recording_dir(
            recording_dir,
            signal="leader-audio",
            local_signal="tone-440" if peer_index == 0 else "silence",
            remote_signal="silence" if peer_index == 0 else "tone-440",
            ignore_pop_events=True,
        )
        local_metro = analyze_metronome_wav(
            recording_dir, allow_silent=peer_index != 0)
        analysis["metronome"] = local_metro
        received = None
        if peer_index == 0:
            tone_probe = _steady_tone(recording_dir, "my-input")
            analysis["leader_audio_tone"] = tone_probe
            if not tone_probe.get("ok", False):
                analysis["tags"].append("leader_local_tone_steady_probe_failed")
            if not local_metro.get("ok", False) or local_metro.get("actual_clicks", 0) < 16:
                analysis["tags"].append(
                    local_metro.get("verdict", "leader_local_clicks_insufficient"))
        else:
            received = _analyze_received_leader_audio(recording_dir)
            analysis["leader_audio_received"] = received
            tone_probe = _steady_tone(recording_dir, "their-input")
            analysis["leader_audio_tone"] = tone_probe
            if local_metro.get("clicks", 0) != 0:
                analysis["tags"].append("listener_local_metronome_not_silent")
            if not received.get("ok", False) or received.get("actual_clicks", 0) < 16:
                analysis["tags"].append(
                    received.get("verdict", "leader_audio_clicks_insufficient"))
            if not tone_probe.get("ok", False):
                analysis["tags"].append("listener_leader_tone_steady_probe_failed")
        click_count = int(
            local_metro.get("actual_clicks", 0) if peer_index == 0
            else (received or {}).get("actual_clicks", 0))
        intentional_clipping = {}
        expected_clipped_stems = (
            ("metronome", "mix") if peer_index == 0
            else ("their-input", "inputs-mix", "mix"))
        for stem in expected_clipped_stems:
            clipped = int(analysis.get("stems", {}).get(stem, {}).get("clipped_frames", 0))
            tag = f"{stem}_clipping_detected"
            if (stem == "mix" or clipped <= click_count) and tag in analysis["tags"]:
                analysis["tags"].remove(tag)
                intentional_clipping[stem] = clipped
        analysis["intentional_click_clipping_frames"] = intentional_clipping
        analysis["ok"] = not analysis["tags"]
        return analysis

    analysis = analyze_recording_dir(
        recording_dir,
        signal="metro-pulse" if case_id.endswith("metro-pulse") else "silence",
        ignore_pop_events=True,
    )
    metro = analyze_metronome_wav(recording_dir)
    analysis["metronome"] = metro
    # The deterministic click's single-sample peak intentionally reaches PCM16
    # full scale. Treat at most one such sample per detected click in the local
    # metronome and resulting mix as the click shape, not arbitrary clipping.
    click_count = int(metro.get("actual_clicks", 0))
    intentional_clipping = {}
    for stem in ("metronome", "mix"):
        clipped = int(analysis.get("stems", {}).get(stem, {}).get("clipped_frames", 0))
        tag = f"{stem}_clipping_detected"
        # The output mix can also saturate where a deterministic metro-pulse
        # overlaps the full-scale click. Source stems remain independently
        # checked, so mix-only clipping is expected for this constructed case.
        expected = stem == "mix" or clipped <= click_count
        if expected and tag in analysis["tags"]:
            analysis["tags"].remove(tag)
            intentional_clipping[stem] = clipped
    analysis["intentional_click_clipping_frames"] = intentional_clipping
    if case_id.endswith("metro-pulse"):
        listener_clicks_ok = (
            metro.get("actual_clicks", 0) >= 16 and
            metro.get("max_interval_error_frames", 999999) <= 1200
        )
        if not listener_clicks_ok:
            analysis["tags"].append("listener_compensated_click_interval_or_count_failed")
    elif not metro.get("ok", False) or metro.get("actual_clicks", 0) < 16:
        analysis["tags"].append(metro.get("verdict", "metronome_clicks_insufficient"))
    if (case_id == "public-cli-metronome-shared-grid" and
            metro.get("max_interval_error_frames", 999) > 240):
        analysis["tags"].append("shared_grid_click_interval_not_tight")
    if case_id.endswith("metro-pulse"):
        epoch = analyze_metro_pulse_epoch_side(recording_dir)
        listener = analyze_listener_compensated_pulse_side(recording_dir)
        listener_ok = (
            listener.get("ok", False) and
            listener.get("steady_samples", 0) >= 10 and
            listener.get("missing_pulse_matches", 0) == 0
        )
        analysis["metro_pulse_epoch"] = epoch
        analysis["listener_compensated_pulse"] = listener
        if not epoch.get("ok", False):
            analysis["tags"].append(epoch.get("verdict", "metro_pulse_epoch_failed"))
        if not listener_ok:
            analysis["tags"].append(listener.get("verdict", "listener_pulse_timing_failed"))
    analysis["ok"] = not analysis["tags"]
    return analysis


def _public_case_console_summary(case_id: str, result: dict[str, Any]) -> str:
    metrics = result["mesh_metrics"]
    if case_id == "public-cli-network-clean":
        return (
            f"peers=4 active={metrics['network_active_peer_count_established_min']} "
            f"sent={int(metrics['sent_packets_total'])} recv={int(metrics['recv_packets_total'])}"
        )
    analyses = [peer["audio_analysis"] for peer in result["peer_results"]]
    if case_id == "public-cli-network-tone":
        frequencies = [
            probe["tone"]["estimated_hz"]
            for analysis in analyses
            for probe in analysis.get("validation_probes", [])
            if "tone" in probe and probe["tone"].get("tone_present", False)
        ]
        pulses = [
            probe["count"]
            for analysis in analyses
            for probe in analysis.get("validation_probes", [])
            if "count" in probe
        ]
        return (
            f"tone={min(frequencies, default=0.0):.1f}-{max(frequencies, default=0.0):.1f}Hz "
            f"pulses={min(pulses, default=0)}-{max(pulses, default=0)}"
        )
    if case_id == "public-cli-metronome-shared-grid":
        metro = [analysis["metronome"] for analysis in analyses]
        clicks = [item.get("actual_clicks", 0) for item in metro]
        max_frames = max((item.get("max_interval_error_frames", 0) for item in metro), default=0)
        mapping_frames = result.get("shared_grid_alignment", {}).get(
            "max_abs_mapping_error_frames", 0.0)
        return (
            f"clicks={min(clicks, default=0)}-{max(clicks, default=0)} "
            f"max_interval_error={max_frames * 1000.0 / 48000.0:.1f}ms "
            f"max_grid_error={mapping_frames * 1000.0 / 48000.0:.1f}ms"
        )
    if case_id == "public-cli-metronome-leader-audio":
        received = [
            analysis["leader_audio_received"] for analysis in analyses[1:]
        ]
        clicks = [item.get("actual_clicks", 0) for item in received]
        max_frames = max(
            (item.get("max_interval_error_frames", 0) for item in received), default=0)
        frequencies = [
            analysis.get("leader_audio_tone", {}).get("tone", {}).get("estimated_hz", 0.0)
            for analysis in analyses
        ]
        return (
            f"leader_clicks={min(clicks, default=0)}-{max(clicks, default=0)} "
            f"max_interval_error={max_frames * 1000.0 / 48000.0:.1f}ms "
            f"tone={min(frequencies, default=0.0):.1f}-{max(frequencies, default=0.0):.1f}Hz "
            f"source_peers=1"
        )
    listeners = [analysis["listener_compensated_pulse"] for analysis in analyses]
    contracts = [analysis.get("listener_compensation_contract", {}) for analysis in analyses]
    samples = [item.get("steady_samples", 0) for item in listeners]
    max_error = max((item.get("steady_max_abs_error_ms", 0.0) for item in listeners), default=0.0)
    average_latencies = [item.get("average_latency_ms", 0.0) for item in contracts]
    convergence_frames = [item.get("convergence_error_frames", 0.0) for item in contracts]
    return (
        f"steady_matches={min(samples, default=0)}-{max(samples, default=0)} "
        f"average_latency={min(average_latencies, default=0.0):.1f}-"
        f"{max(average_latencies, default=0.0):.1f}ms "
        f"max_landing_error={max_error:.1f}ms "
        f"max_target_error={max(convergence_frames, default=0.0) * 1000.0 / 48000.0:.1f}ms"
    )


def _public_case_failure(result: dict[str, Any]) -> str:
    verdict = result.get("verdict", "unknown_failure")
    for peer in result.get("peer_results", []):
        if peer.get("return_code") != 0:
            return f"peer={peer['peer']} reason=process_exit_{peer.get('return_code')}"
        if peer.get("timed_out"):
            return f"peer={peer['peer']} reason=process_timeout"
        if not peer.get("startup_ok", False):
            return f"peer={peer['peer']} reason=startup_contract_failed"
        if not peer.get("edge_contract_ok", False):
            return f"peer={peer['peer']} reason=full_mesh_edges_incomplete"
        audio = peer.get("audio_analysis", {})
        if not audio.get("ok", False):
            tags = ",".join(audio.get("tags", [])) or "audio_analysis_failed"
            return f"peer={peer['peer']} reason={tags}"
    return f"reason={verdict}"


def _run_public_network_case(
    jam2: Path,
    invocation: InvocationArtifacts,
    manifest: InvocationManifest,
    case_id: str,
    reporter: ValidationReporter,
) -> bool:
    reporter.start(case_id)
    root = invocation.root / case_id
    root.mkdir()
    ports = _reserve_unique_dual_ports(4)
    session_id = f"{secrets.randbits(64) or 1:016x}"
    session_key = secrets.token_hex(16)
    metronome = case_id.startswith("public-cli-metronome-")
    listener_mode = case_id.endswith("metro-pulse")
    leader_mode = case_id.endswith("leader-audio")
    signals = (
        ("tone-440", "pulse-1s", "silence", "silence")
        if case_id == "public-cli-network-tone" else
        ("tone-440", "silence", "silence", "silence")
        if leader_mode else
        (("metro-pulse",) * 4 if listener_mode else ("silence",) * 4)
    )
    environment = os.environ.copy()
    for name in (
        "JAM2_AUTOMATION_COMMAND_HANDLE", "JAM2_AUTOMATION_EVENT_HANDLE",
        "JAM2_AUTOMATION_COMMAND_FD", "JAM2_AUTOMATION_EVENT_FD",
    ):
        environment.pop(name, None)

    processes: list[dict[str, Any]] = []

    def start_peer(index: int, command: list[str]) -> subprocess.Popen[str]:
        peer_root = root / f"peer-{index}"
        peer_root.mkdir()
        stdout_path = peer_root / "stdout.log"
        stderr_path = peer_root / "stderr.log"
        stdout = stdout_path.open("w", encoding="utf-8", newline="")
        stderr = stderr_path.open("w", encoding="utf-8", newline="")
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            text=True,
            env=environment,
        )
        processes.append({
            "peer": f"peer-{index}", "peer_root": peer_root,
            "process": process, "stdout": stdout, "stderr": stderr,
            "stdout_path": stdout_path, "command": command,
        })
        return process

    def common(index: int, stream_ms: int) -> list[str]:
        peer_root = root / f"peer-{index}"
        return [
            "--profile", "fast",
            "--headless-audio", "on",
            "--sample-rate", "48000",
            "--audio-buffer-size", "256",
            "--frame-size", "64",
            "--network-audio-format", "pcm16",
            "--test-input", signals[index],
            "--metronome", "on" if metronome else "off",
            "--bpm", "120",
            "--metronome-mode", (
                "listener-compensated" if listener_mode else
                "leader-audio" if leader_mode else "shared-grid"),
            "--stats", "enabled",
            "--stats-interval-ms", "250",
            "--stats-warmup-ms", "1000",
            "--log-stats", str(peer_root / "csv"),
            "--record-jam-folder", str(peer_root / "recording"),
            "--stream-ms", str(stream_ms),
            "--stream-linger-ms", "100",
        ]

    infrastructure_error = ""
    try:
        creator_command = [
            str(jam2), "network", "create",
            "--bind", f"127.0.0.1:{ports[0]}",
            "--public-endpoint", f"127.0.0.1:{ports[0]}",
            "--no-stun",
            "--wait-ms", "10000",
            "--session-id", session_id,
            "--session-key", session_key,
            "--max-peers", "3",
            *common(0, PUBLIC_NETWORK_STREAM_MS + 1000),
        ]
        creator = start_peer(0, creator_command)
        invite = _wait_for_creator_invite(creator, processes[0]["stdout_path"], 5.0)
        for index in range(1, 4):
            command = [
                str(jam2), "network", "join", invite,
                "--bind", f"127.0.0.1:{ports[index]}",
                "--wait-ms", "10000",
                *common(index, PUBLIC_NETWORK_STREAM_MS),
            ]
            start_peer(index, command)
        for item in processes:
            code, timed_out = _wait_process(item["process"], 22.0)
            item["return_code"] = code
            item["timed_out"] = timed_out
            item["stdout"].close()
            item["stderr"].close()
    except Exception as error:
        infrastructure_error = f"{type(error).__name__}: {error}"
    finally:
        for item in processes:
            process = item["process"]
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            item.setdefault("return_code", process.returncode)
            item.setdefault("timed_out", False)
            if not item["stdout"].closed:
                item["stdout"].close()
            if not item["stderr"].closed:
                item["stderr"].close()

    peer_results = []
    for index, item in enumerate(processes):
        peer_root = item["peer_root"]
        csv_files = sorted((peer_root / "csv").glob("*.csv"))
        csv_summary = summarize_csv(
            csv_files[0] if len(csv_files) == 1 else peer_root / "missing.csv",
            assessment_elapsed_ms=PUBLIC_NETWORK_STREAM_MS,
        )
        try:
            audio = _public_audio_analysis(case_id, index, peer_root / "recording")
        except Exception as error:
            audio = {"ok": False, "tags": [f"analysis_error:{type(error).__name__}:{error}"]}
        if listener_mode and csv_summary.get("has_csv", False):
            compensation = _listener_compensation_contract(csv_summary, audio, 3)
            audio["listener_compensation_contract"] = compensation
            if not compensation["ok"]:
                failed_checks = ",".join(
                    name for name, passed in compensation["checks"].items() if not passed)
                audio.setdefault("tags", []).append(
                    f"listener_compensation_contract_failed:{failed_checks}")
                audio["ok"] = False
        startup = _startup_events(item["stdout_path"])
        stages = [event.get("stage") for event in startup]
        startup_ok = (
            (index == 0 and "waiting" in stages and stages.count("connected") >= 3) or
            (index > 0 and "connecting" in stages and "connected" in stages)
        )
        edges = _mesh_edge_summaries(item["stdout_path"])
        stdout_edge_contract_ok = (
            len(edges) == 3 and all(
                edge.get("endpoint_proof_verified", False) and
                edge.get("sent_packets", 0) > 0 and
                edge.get("recv_packets", 0) > 0
                for edge in edges.values()
            )
        )
        csv_edge_contract_ok = _csv_full_mesh_contract(csv_summary, 3)
        edge_contract_ok = stdout_edge_contract_ok or csv_edge_contract_ok
        peer_results.append({
            "peer": item["peer"],
            "role": "coordinator" if index == 0 else "peer",
            "return_code": item["return_code"],
            "timed_out": item["timed_out"],
            "csv_path": str(csv_files[0]) if len(csv_files) == 1 else "",
            "csv_summary": csv_summary,
            "audio_analysis": audio,
            "startup_events": startup,
            "startup_ok": startup_ok,
            "mesh_edges": list(edges.values()),
            "edge_evidence": "stdout" if stdout_edge_contract_ok else
                "structured_csv" if csv_edge_contract_ok else "incomplete",
            "edge_contract_ok": edge_contract_ok,
        })

    result: dict[str, Any] = {
        "schema": "jam2-public-cli-validation-result",
        "case": case_id,
        "requested_stream_ms": PUBLIC_NETWORK_STREAM_MS,
        "effective_stream_ms": PUBLIC_NETWORK_STREAM_MS,
        "requested_network_audio_format": "pcm16",
        "headless_clock_drift_ppm": [0, 0, 0, 0],
        "topology": {"peers": 4, "creator": 1, "joiners": 3, "engine": "public-cli-direct-mesh"},
        "infrastructure_error": infrastructure_error,
        "peer_results": peer_results,
    }
    result["mesh_metrics"] = mesh_collect_metrics(
        peer_results, requested_stream_ms=PUBLIC_NETWORK_STREAM_MS)
    result["verdict"] = mesh_verdict(result)
    if case_id == "public-cli-metronome-shared-grid" and result["verdict"] == "pass":
        mapping_errors = [
            abs(peer.get("csv_summary", {}).get("grid_mapping_error_frames", 999.0))
            for peer in peer_results
        ]
        result["shared_grid_alignment"] = {
            "max_abs_mapping_error_frames": max(mapping_errors, default=999.0),
            "tolerance_frames": 240,
        }
        if not mapping_errors or max(mapping_errors) > 240:
            result["verdict"] = "shared_grid_mapping_not_tight"
    if case_id == "public-cli-metronome-leader-audio" and result["verdict"] == "pass":
        creator_id = (
            peer_results[0].get("csv_summary", {}).get("local_peer_id", 0)
            if peer_results else 0)
        metrics = result["mesh_metrics"]
        if (metrics.get("leader_audio_injected_peers", 0) != 1 or
                metrics.get("leader_audio_source_peer_ids", []) != [creator_id]):
            result["verdict"] = "leader_audio_source_invalid"
    if not infrastructure_error and not all(
            peer.get("startup_ok", False) and peer.get("edge_contract_ok", False)
            for peer in peer_results):
        if result["verdict"] == "pass":
            result["verdict"] = "public_cli_lifecycle_or_edge_contract_failed"
    passed = (
        not infrastructure_error and len(peer_results) == 4 and
        result["verdict"] == "pass" and
        all(peer.get("startup_ok", False) and peer.get("edge_contract_ok", False)
            for peer in peer_results)
    )
    (root / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    summary = _public_case_console_summary(case_id, result) if peer_results else ""
    failure = (
        f"reason={infrastructure_error}" if infrastructure_error
        else _public_case_failure(result)
    )
    return _case(
        manifest,
        reporter,
        case_id,
        "passed" if passed else "failed",
        console_summary=summary,
        failure_detail=f"{failure} artifacts={root}",
        result=result,
    )


def _coverage_map(capabilities: NativeCapabilities, jam2: Path, root: Path) -> dict[str, Any]:
    schema_path = Path(__file__).resolve().parents[1] / "scenarios" / "jam2-debug-scenario.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    help_text = ""
    for args in (["-h"], ["local", "-h"], ["network", "create", "-h"], ["network", "join", "-h"]):
        result = subprocess.run([str(jam2), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20, check=False)
        help_text += result.stdout + "\n"
    public_options = sorted(set(re.findall(r"(?<![\w-])--[a-z][a-z0-9-]*", help_text)))
    manual_options = {"--audio-device", "--input-channels", "--output-channels"}
    runtime_options = {
        "--" + item["name"].replace("_", "-")
        for item in capabilities.description["runtime_fields"]
    }
    bootstrap_options = {
        "--bind", "--public-endpoint", "--no-stun", "--stun",
        "--stun-timeout-ms", "--stun-retries", "--wait-ms", "--max-peers",
        "--session-id", "--session-key",
    }

    def public_coverage(option: str) -> dict[str, str]:
        if option in manual_options:
            return {"coverage": "optional-device/manual-only",
                    "case": "real-device-extension or explicit channel selection"}
        if option == "--help":
            return {"coverage": "automated", "case": "public-command-surface"}
        if option in bootstrap_options:
            return {"coverage": "automated", "case": "clean-mesh-2/3/4 and effective configuration"}
        if option == "--log-stats":
            return {"coverage": "automated", "case": "every debug-run artifact contract"}
        if option == "--record-jam-folder":
            return {"coverage": "retained-catalog", "case": "recording.start/stop stress and artifact analysis"}
        if option in runtime_options or option == "--profile":
            return {"coverage": "automated-or-native-validator",
                    "case": "effective configuration, focused boundaries, and stress/benchmark matrices"}
        return {"coverage": "unclassified", "case": "none"}

    public_entries = [{"name": option, **public_coverage(option)} for option in public_options]
    scenario_properties = schema["properties"]
    scenario_field_names = set(scenario_properties)
    for section in ("runtime", "network", "artifacts", "automation"):
        scenario_field_names.update(
            f"{section}.{name}"
            for name in scenario_properties[section]["properties"]
        )
    scenario_field_names.update(
        f"actions[].{name}"
        for name in scenario_properties["actions"]["items"]["properties"]
    )
    field_cases = {
        "schema": "description-schema-operation-parity",
        "run_id": "every debug-run native manifest",
        "operation": "focused/local/clean-mesh operations",
        "profile": "local effective configuration and stress/benchmark matrices",
        "runtime": "local effective configuration and native boundaries",
        "network": "clean-mesh, stress, and benchmark create/join",
        "artifacts": "native and invocation artifact manifests",
        "actions": "scheduled-control validation and retained stress catalog",
        "automation": "reactive-inherited-channel",
        "fixtures": "focused boundaries and prepared-track stress",
        "network.bind": "clean-mesh create/join",
        "network.join_url": "clean-mesh join",
        "network.no_stun": "clean-mesh local transport",
        "network.stun": "native option validator and connectivity-stun",
        "network.stun_timeout_ms": "native option validator and connectivity-stun",
        "network.stun_retries": "native option validator and connectivity-stun",
        "network.wait_ms": "clean-mesh startup timeout",
        "network.public_endpoint": "two-host benchmark coordinator",
        "network.session_id": "clean-mesh and benchmark create",
        "network.session_key": "clean-mesh and benchmark create",
        "network.max_peers": "clean-mesh and benchmark create",
        "network.heartbeat_interval_ms": "real-process lifecycle with native override",
        "network.heartbeat_miss_limit": "real-process lifecycle with native override",
        "network.peer_token": "reactive, stress, and benchmark peers",
        "network.topology": "retained impairment stress edges",
        "artifacts.root": "every debug-run artifact contract",
        "automation.reactive": "reactive-inherited-channel",
        "automation.controller_loss": "reactive-inherited-channel",
        "actions[].id": "scheduled-control validation and retained stress catalog",
        "actions[].type": "action coverage map and retained stress catalog",
        "actions[].after_event": "scheduled-control validation and retained stress catalog",
        "actions[].delay_frames": "scheduled-control validation and retained stress catalog",
        "actions[].apply_frame": "clean-mesh creator absolute-frame snapshot",
        "actions[].enabled": "clean-mesh metronome and track-sync stress",
        "actions[].value": "clean-mesh BPM and runtime-control stress",
        "actions[].mode": "retained metronome-mode stress",
        "actions[].path": "prepared-track and recording stress/benchmark",
        "actions[].count_in_bars": "retained record-start stress",
    }
    scenario_entries = []
    for name in sorted(scenario_field_names):
        if name.startswith("runtime."):
            scenario_entries.append({
                "name": name, "coverage": "automated-or-native-validator",
                "case": "effective-configuration/native-boundaries/stress-matrices",
            })
        elif name in field_cases:
            scenario_entries.append({"name": name, "coverage": "automated-or-retained", "case": field_cases[name]})
        else:
            scenario_entries.append({"name": name, "coverage": "unclassified", "case": "none"})
    return {
        "schema": "jam2-validation-coverage-map",
        "public_cli_options": public_entries,
        "debug_operations": [{"name": value, "coverage": "automated", "case": "focused/local/clean-mesh"} for value in capabilities.description["operations"]],
        "automation_actions": [
            {"name": value, "coverage": "retained-stress-catalog", "case": ACTION_CASES[value]}
            for value in capabilities.description["actions"]
        ],
        "runtime_fields": [
            {"name": value["name"], "coverage": "device/manual-only" if value["name"] in {"audio_device", "input_channels", "output_channels"} else "automated-or-native-validator", "case": "effective-configuration/native-boundaries"}
            for value in capabilities.description["runtime_fields"]
        ],
        "protocol_fields": [
            {"name": name, "coverage": "automated", "case": "description/schema/reactive-channel"}
            for name in ("schema", "scenario_schema", "automation_protocol", "control_protocol_version", "udp_protocol_version", "max_automation_frame_bytes", "automation_queue_capacity", "automation_commands_per_turn")
        ],
        "scenario_fields": scenario_entries,
        "representative_boundaries": ["frame_size below minimum", "unknown runtime field", "obsolete local format", "native focused boundary corpus"],
        "explicit_omissions": ["physical audio devices", "GUI-only interaction"],
        "unclassified_public_cli_options": [
            item["name"] for item in public_entries if item["coverage"] == "unclassified"
        ],
        "unclassified_scenario_fields": [
            item["name"] for item in scenario_entries if item["coverage"] == "unclassified"
        ],
    }


def run(
    selection: str,
    repo: Path,
    jam2: Path,
    invocation: InvocationArtifacts,
    manifest: InvocationManifest,
    real_device: str | None = None,
) -> int:
    passed = True
    infrastructure_error: str | None = None
    product_selected = selection in ("all", "product")
    total = (1 if selection in ("all", "framework") else 0) + (17 if product_selected else 0)
    if product_selected and real_device:
        total += 1
    reporter = ValidationReporter(total, invocation.root)
    try:
        capabilities = NativeCapabilities(jam2)
        (invocation.root / "debug-description.json").write_text(json.dumps(capabilities.description, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        coverage = _coverage_map(capabilities, jam2, invocation.root)
        (invocation.root / "coverage-map.json").write_text(json.dumps(coverage, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        manifest.data["coverage_map"] = "coverage-map.json"
        manifest.data["native_profiles"] = sorted(capabilities.profiles)
        omissions = list(coverage["explicit_omissions"])
        if real_device:
            omissions = [value for value in omissions if value != "physical audio devices"]
        manifest.data["omissions"] = omissions
        manifest.write()
        if (coverage["unclassified_public_cli_options"] or
                coverage["unclassified_scenario_fields"]):
            raise RuntimeError(
                "validation coverage map has unclassified surfaces: " +
                ", ".join(coverage["unclassified_public_cli_options"] +
                          coverage["unclassified_scenario_fields"]))
        if selection in ("all", "framework"):
            passed &= _run_framework(repo, invocation, manifest, reporter)
        if product_selected:
            passed &= _run_help_contract(jam2, invocation, manifest, reporter)
            passed &= _run_schema_parity(repo, capabilities, manifest, reporter)
            passed &= _run_focused(jam2, invocation, manifest, reporter)
            passed &= _run_real_process_control_hardening(jam2, invocation, manifest, reporter)
            passed &= _run_local(jam2, invocation, manifest, reporter)
            passed &= _run_invalid_contracts(jam2, invocation, manifest, reporter)
            passed &= _run_reactive_channel(jam2, invocation, manifest, reporter)
            passed &= _run_handle_isolation(jam2, invocation, manifest, reporter)
            for peers in (2, 3, 4):
                passed &= _run_mesh(jam2, invocation, manifest, peers, reporter)
            for case_id in PUBLIC_NETWORK_CASES:
                passed &= _run_public_network_case(
                    jam2, invocation, manifest, case_id, reporter)
            if real_device:
                passed &= _run_real_device(
                    jam2, invocation, manifest, real_device, reporter)
    except Exception as error:
        infrastructure_error = f"{type(error).__name__}: {error}"
        passed = False
        manifest.add_case({"id": "validation-infrastructure", "status": "infrastructure-error", "error": infrastructure_error})
        reporter.fail_setup(infrastructure_error)
    status = "passed" if passed else ("infrastructure-error" if infrastructure_error else "failed")
    code = 0 if passed else (2 if infrastructure_error else 1)
    manifest.finish(status, code, selection=selection)
    reporter.summary(passed, infrastructure_error)
    return code
