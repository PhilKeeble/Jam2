from __future__ import annotations

import json
import os
import re
import secrets
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from .artifacts import InvocationArtifacts
from .manifest import InvocationManifest
from .native import NativeCapabilities, ReactiveProcess, native_manifest, run_logged, write_scenario


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


def _case(manifest: InvocationManifest, name: str, status: str, **evidence: Any) -> bool:
    manifest.add_case({"id": name, "status": status, **evidence})
    return status == "passed"


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


def _run_framework(repo: Path, invocation: InvocationArtifacts, manifest: InvocationManifest) -> bool:
    case_root = invocation.root / "framework"
    case_root.mkdir()
    command = [sys.executable, "-m", "unittest", "discover", "-s", str(repo / "tools"), "-p", "test_*.py"]
    result = run_logged(command, case_root / "stdout.log", case_root / "stderr.log", timeout=120)
    return _case(manifest, "framework-self-tests", "passed" if result["return_code"] == 0 else "failed", process=result)


def _run_help_contract(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest) -> bool:
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
    removed = run_logged(
        [str(jam2), "mesh", "-h"], case_root / "removed.stdout.log",
        case_root / "removed.stderr.log", timeout=20,
    )
    ok &= removed["return_code"] != 0
    return _case(manifest, "public-command-surface", "passed" if ok else "failed", processes=results, removed_mesh=removed)


def _run_focused(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest) -> bool:
    ok = True
    for operation in ("validate.boundaries", "validate.controller-lifecycle"):
        name = operation.replace(".", "-")
        root = invocation.root / name
        root.mkdir()
        scenario = _base_scenario(name, operation, root)
        scenario.pop("runtime")
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
        ok &= _case(manifest, name, "passed" if passed else "failed", process=result, native=native)
    return ok


def _run_schema_parity(repo: Path, capabilities: NativeCapabilities,
                       manifest: InvocationManifest) -> bool:
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
        manifest, "description-schema-operation-parity",
        "passed" if passed else "failed",
        schema_operations=schema_operations,
        described_operations=described_operations,
        runtime_field_count=len(schema_runtime_fields),
        action_count=len(schema_actions),
    )


def _run_local(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest) -> bool:
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
    return _case(manifest, "local-effective-configuration", "passed" if passed else "failed", process=result, native=native)


def _run_real_device(jam2: Path, invocation: InvocationArtifacts,
                     manifest: InvocationManifest, device_text: str) -> bool:
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
    return _case(manifest, "real-device-extension", "passed" if passed else "failed",
                 process=result, native=native,
                 coverage="explicit physical-device extension; headless baseline also ran")


def _run_invalid_contracts(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest) -> bool:
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
    return _case(manifest, "schema-and-numeric-rejection", "passed" if ok else "failed", processes=results)


def _run_reactive_channel(jam2: Path, invocation: InvocationArtifacts,
                          manifest: InvocationManifest) -> bool:
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
    return _case(manifest, "reactive-inherited-channel", "passed" if passed else "failed",
                 return_code=code, events=[event.get("event") for event in events],
                 python_event_drops=process.event_drops, native=native,
                 controller_loss_policies=loss_results)


def _run_handle_isolation(jam2: Path, invocation: InvocationArtifacts,
                          manifest: InvocationManifest) -> bool:
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
    return _case(manifest, "automation-handle-isolation", "passed" if passed else "failed",
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


def _run_mesh(jam2: Path, invocation: InvocationArtifacts, manifest: InvocationManifest, peers: int) -> bool:
    case_id = f"clean-mesh-{peers}-peers"
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
            except Exception as error:
                native = {"error": str(error)}
                remote_count = None
                native_ok = False
            passed &= code == 0 and not timed_out and native_ok
            outcomes.append({"peer": peer_root.name, "return_code": code, "timed_out": timed_out, "remote_peer_count": remote_count, "native": native})
    finally:
        for process, stdout, stderr, _ in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
            if not stdout.closed: stdout.close()
            if not stderr.closed: stderr.close()
    return _case(manifest, case_id, "passed" if passed else "failed", topology={"peers": peers, "engine": "universal-direct-mesh"}, processes=outcomes)


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
        if option == "--grid-coordinator":
            return {"coverage": "native-validator/retained-catalog", "case": "focused boundaries and grid authority stress"}
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
        "explicit_omissions": ["physical audio devices", "GUI-only interaction", "Phase 11 lifecycle/endpoint/mixed-device/WAV/adversarial additions"],
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
            passed &= _run_framework(repo, invocation, manifest)
        if selection in ("all", "product"):
            passed &= _run_help_contract(jam2, invocation, manifest)
            passed &= _run_schema_parity(repo, capabilities, manifest)
            passed &= _run_focused(jam2, invocation, manifest)
            passed &= _run_local(jam2, invocation, manifest)
            passed &= _run_invalid_contracts(jam2, invocation, manifest)
            passed &= _run_reactive_channel(jam2, invocation, manifest)
            passed &= _run_handle_isolation(jam2, invocation, manifest)
            for peers in (2, 3, 4):
                passed &= _run_mesh(jam2, invocation, manifest, peers)
            if real_device:
                passed &= _run_real_device(jam2, invocation, manifest, real_device)
    except Exception as error:
        infrastructure_error = f"{type(error).__name__}: {error}"
        passed = False
        manifest.add_case({"id": "validation-infrastructure", "status": "infrastructure-error", "error": infrastructure_error})
    status = "passed" if passed else ("infrastructure-error" if infrastructure_error else "failed")
    code = 0 if passed else (2 if infrastructure_error else 1)
    manifest.finish(status, code, selection=selection)
    return code
