from __future__ import annotations

import csv
import json
import re
import secrets
import shutil
import socket
import subprocess
import threading
import time
import traceback
import uuid
import zipfile
from pathlib import Path
from typing import Any

from .artifacts import (
    InvocationArtifacts, adopt_invocation, allocate_invocation,
    benchmark_attempt_path, normalized_path_id, validate_native_attempt_root,
)
from .audio_analysis import analyze_recording_dir
from .benchmark_control import (
    BenchmarkControlClient, new_suite_id, parse_host_port, run_identity,
    same_run, start_control_server,
)
from .benchmark_suite import benchmark_cases
from .manifest import InvocationManifest
from .metrics import normalized_pair_summary, summarize_csv
from .native import NativeCapabilities, native_manifest, write_scenario
from .profiles import configure_native_profiles


MAX_ARCHIVE_FILES = 1024
MAX_ARCHIVE_UNCOMPRESSED_BYTES = 2 * 1024 * 1024 * 1024
MAX_ARCHIVE_SOURCE_BYTES = 1024 * 1024 * 1024
MAX_ANALYSIS_RESULTS = 65536
MAX_ANALYSIS_SOURCE_BYTES = 1024 * 1024 * 1024
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
SAFE_CASE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,256}$")
BENCHMARK_SIGNALS = {"silence", "tone-440", "pulse-1s"}


def _log(path: Path, text: str) -> None:
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {text}"
    print(line, flush=True)
    with path.open("a", encoding="utf-8") as handle: handle.write(line + "\n")


class _RunLog:
    def __init__(self, path: Path):
        self.path = path
        self.path.touch(exist_ok=True)
        self._lock = threading.Lock()
        self._frozen = False

    def __call__(self, text: str) -> None:
        line = f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {text}"
        with self._lock:
            if not self._frozen:
                with self.path.open("a", encoding="utf-8") as handle:
                    handle.write(line + "\n")
        print(line, flush=True)

    def freeze(self) -> None:
        with self._lock:
            self._frozen = True


def _validate_common_arguments(args: Any) -> None:
    if not SAFE_ID.fullmatch(args.machine_id):
        raise ValueError("--machine-id must use 1..64 letters, numbers, dot, underscore, or hyphen")
    if not 1 <= args.sample_rate <= 768000:
        raise ValueError("--sample-rate is outside the native bound")
    if not 100 <= args.stream_ms <= 86_400_000:
        raise ValueError("--stream-ms must be from 100 ms through 24 hours")
    if not 1 <= args.repeats <= 100:
        raise ValueError("--repeats must be from 1 through 100")
    if args.case_timeout_s < 0 or args.case_timeout_s > 86_500:
        raise ValueError("--case-timeout-s is outside its bound")
    if len(args.case) > 256 or any(not SAFE_CASE_ID.fullmatch(value) for value in args.case):
        raise ValueError("--case selections must be bounded case identifiers")
    if args.audio_device is not None:
        try:
            device_id = int(args.audio_device)
        except ValueError as error:
            raise ValueError("--audio-device must be a numeric native device identifier") from error
        if not 0 <= device_id <= 65535:
            raise ValueError("--audio-device is outside the native bound")


def _correlated_process_outcome(
    coordinator_return_code: int,
    agent_result: dict[str, Any],
) -> tuple[str, bool]:
    succeeded = (
        coordinator_return_code == 0 and
        agent_result.get("return_code") == 0
    )
    return ("complete" if succeeded else "process-failed", succeeded)


def _agent_attempt_status(upload_acknowledged: bool, return_code: int) -> str:
    return "passed" if upload_acknowledged and return_code == 0 else "failed"


def _failure_manifest(
    repo: Path,
    args: Any,
    arguments: list[str],
    run_id: str,
    case_id: str,
    error: Exception | str,
    clean: bool = False,
) -> int:
    failed = allocate_invocation(
        "benchmark", repo / "tools", args.output, run_id, clean)
    manifest = InvocationManifest(
        failed.root / "invocation-manifest.json", "benchmark",
        failed.invocation_id, arguments)
    message = str(error)
    manifest.add_case({"id": case_id, "status": "infrastructure-error", "error": message})
    manifest.finish("infrastructure-error", 2, error=message)
    print(f"[benchmark] artifacts: {failed.root}", flush=True)
    return 2


def _scenario(case: Any, operation: str, run_id: str, root: Path,
              audio_device: str, sample_rate: int, bind: str, invite: str,
              session_id: str, session_key: str, public_endpoint: str,
              side: str, machine_id: str) -> dict[str, Any]:
    signal = case.coordinator_signal if side == "coordinator" else case.agent_signal
    runtime = case.profile.runtime(case.stream_ms)
    # The retained benchmark matrix records a common audible/grid reference;
    # native profile defaults still come only from debug describe.
    runtime.setdefault("metronome", True)
    runtime.setdefault("bpm", 120)
    runtime.setdefault("metronome_level", 0.20)
    runtime.update({
        "sample_rate": sample_rate,
        "stream_ms": 0,
        "test_input": "silence" if signal == "metronome-only" else signal,
    })
    if audio_device is None:
        runtime.update({"headless_audio": True, "audio_buffer_size": 256})
    else:
        runtime["audio_device"] = int(audio_device)
    recording = root / "recording"
    stop_frame = max(sample_rate, round((case.stream_ms / 1000.0 - 1.0) * sample_rate))
    result: dict[str, Any] = {
        "schema": "jam2-debug-scenario", "run_id": run_id,
        "operation": operation, "profile": case.profile.base_profile,
        "runtime": runtime, "artifacts": {"root": str(root)},
        "network": {"bind": bind, "no_stun": True, "wait_ms": case.stream_ms + 60000,
                    "peer_token": secrets.token_hex(16)},
        "actions": [
            {"id": "record-start", "type": "recording.start", "path": str(recording),
             "after_event": "network.connected", "delay_frames": sample_rate // 2},
            {"id": "record-stop", "type": "recording.stop",
             "after_event": "network.connected", "delay_frames": stop_frame},
            {"id": "shutdown", "type": "shutdown",
             "after_event": "network.connected", "delay_frames": round(case.stream_ms * sample_rate / 1000.0)},
        ],
    }
    if operation == "network.create":
        result["network"].update({"session_id": session_id, "session_key": session_key,
                                  "public_endpoint": public_endpoint, "max_peers": 2})
    else:
        result["network"]["join_url"] = invite
    return result


def _start_native(jam2: Path, scenario_path: Path, root: Path) -> tuple[subprocess.Popen[str], Any, Any]:
    stdout = (root / "jam2.stdout.log").open("w", encoding="utf-8", newline="")
    stderr = (root / "jam2.stderr.log").open("w", encoding="utf-8", newline="")
    process = subprocess.Popen([str(jam2), "debug", "run", str(scenario_path)],
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, text=True)
    return process, stdout, stderr


def _wait_native(process: subprocess.Popen[str], timeout_s: float) -> tuple[int, bool]:
    try: return process.wait(timeout=timeout_s), False
    except subprocess.TimeoutExpired:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
        return process.returncode, True


def _csv_path(root: Path, native: dict[str, Any]) -> Path | None:
    for artifact in native.get("artifacts", []):
        path = root / artifact.get("path", "")
        if path.suffix.lower() == ".csv" and path.is_file(): return path
    return None


def _write_peer_result(root: Path, identity: dict[str, Any], machine_id: str,
                       role: str, return_code: int, timed_out: bool,
                       case: Any) -> dict[str, Any]:
    native = native_manifest(root)
    csv_path = _csv_path(root, native)
    csv_summary = summarize_csv(csv_path) if csv_path else {"has_csv": False}
    try:
        local_peer_id = int(native.get("local_peer_id") or csv_summary.get("local_peer_id", 0))
    except (TypeError, ValueError):
        local_peer_id = 0
    analysis = analyze_recording_dir(
        root / "recording", case.signal,
        local_signal=case.coordinator_signal if role == "coordinator" else case.agent_signal,
        remote_signal=case.agent_signal if role == "coordinator" else case.coordinator_signal,
    )
    result = {
        **identity, "machine_id": machine_id, "role": role,
        "local_peer_id": local_peer_id,
        "remote_peer_id": int(csv_summary.get("remote_peer_id", 0) or 0),
        "return_code": return_code, "timed_out": timed_out,
        "native_manifest": "native-manifest.json",
        "effective_configuration": native.get("effective_configuration", {}),
        "csv_path": csv_path.relative_to(root).as_posix() if csv_path else "",
        "analysis": analysis,
    }
    (root / "peer-result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def _zip_attempt(root: Path) -> Path:
    target = root.parent / f"{root.name}.zip"
    files = []
    total = 0
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if len(files) >= MAX_ARCHIVE_FILES:
            raise ValueError("benchmark attempt contains too many artifact files")
        total += path.stat().st_size
        if total > MAX_ARCHIVE_SOURCE_BYTES:
            raise ValueError("benchmark attempt exceeds the 1 GiB packaging bound")
        files.append(path)
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
        for path in sorted(files):
            archive.write(path, path.relative_to(root).as_posix())
    return target


def _extract_upload(archive_path: str | Path, target: Path) -> None:
    total = 0
    with zipfile.ZipFile(archive_path, "r") as archive:
        infos = archive.infolist()
        if len(infos) > MAX_ARCHIVE_FILES: raise ValueError("artifact archive has too many files")
        for info in infos:
            member = Path(info.filename)
            if member.is_absolute() or ".." in member.parts or info.is_dir():
                if info.is_dir(): continue
                raise ValueError(f"unsafe artifact member: {info.filename}")
            total += info.file_size
            if total > MAX_ARCHIVE_UNCOMPRESSED_BYTES:
                raise ValueError("artifact archive exceeds the 2 GiB expanded bound")
        target.mkdir(parents=True, exist_ok=False)
        try:
            for info in infos:
                if info.is_dir(): continue
                destination = target / Path(info.filename)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info, "r") as source, destination.open("wb") as output:
                    shutil.copyfileobj(source, output, 64 * 1024)
        except Exception:
            shutil.rmtree(target, ignore_errors=True)
            raise


def _selected_cases(args: Any, capabilities: NativeCapabilities) -> list[Any]:
    configure_native_profiles(capabilities.description)
    signals = tuple(value.strip() for value in args.signals.split(",") if value.strip())
    if not signals or len(signals) > len(BENCHMARK_SIGNALS) or not set(signals) <= BENCHMARK_SIGNALS:
        raise ValueError("--signals accepts a comma-separated subset of silence,tone-440,pulse-1s")
    cases = benchmark_cases(
        signals=signals, include_metronome=not args.no_metronome_cases,
        stream_ms=args.stream_ms, repeats=args.repeats,
    )
    if args.profile != "all": cases = [case for case in cases if case.profile.base_profile == args.profile]
    if args.case:
        requested = set(args.case)
        cases = [case for case in cases if case.case_id in requested]
        missing = requested - {case.case_id for case in cases}
        if missing: raise ValueError("unknown benchmark cases: " + ", ".join(sorted(missing)))
    if not cases: raise ValueError("benchmark selection produced no cases")
    for case in cases:
        capabilities.validate_sparse_overrides(case.profile.overrides)
    return cases


def _coordinator(args: Any, repo: Path, arguments: list[str]) -> int:
    invocation = allocate_invocation("benchmark", repo / "tools", args.output, "coordinator", args.clean)
    manifest = InvocationManifest(invocation.root / "invocation-manifest.json", "benchmark", invocation.invocation_id, arguments)
    log_path = invocation.root / "coordinator.log"
    try:
        _validate_common_arguments(args)
        if not 0 <= args.case_retry_limit <= 20:
            raise ValueError("--case-retry-limit must be from 0 through 20")
        for value, name in ((args.initial_agent_timeout_s, "--initial-agent-timeout-s"),
                            (args.upload_timeout_s, "--upload-timeout-s"),
                            (args.finish_grace_s, "--finish-grace-s")):
            if not 0.1 <= value <= 86_500:
                raise ValueError(f"{name} is outside its bound")
        for endpoint, name in ((args.control, "--control"), (args.audio_bind, "--audio-bind")):
            host, port = parse_host_port(endpoint)
            if not host or not 1 <= port <= 65535:
                raise ValueError(f"{name} must identify a host and port from 1 through 65535")
        if args.public_audio_host and len(args.public_audio_host) > 255:
            raise ValueError("--public-audio-host exceeds its 255-character bound")
        capabilities = NativeCapabilities(args.jam2)
        cases = _selected_cases(args, capabilities)
    except Exception as error:
        _log(log_path, traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-coordinator-preflight", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    if args.list_cases:
        for case in cases:
            print(f"{case.case_id} base_profile={case.profile.base_profile} "
                  f"overrides={json.dumps(case.profile.overrides, sort_keys=True)} "
                  f"signal={case.signal} repeats={case.repeats}")
        manifest.finish("passed", 0, listed_cases=len(cases))
        return 0
    if (args.audio_device is None and not args.headless_audio) or not args.public_audio_host:
        manifest.finish("infrastructure-error", 2, error="coordinator requires --audio-device (or --headless-audio) and --public-audio-host")
        return 2
    suite_id = new_suite_id()
    try:
        for case in cases:
            validate_native_attempt_root(benchmark_attempt_path(
                invocation, suite_id, args.machine_id, case.case_id,
                "run-100", "0123456789ab", create=False,
            ))
    except Exception as error:
        _log(log_path, traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-path-preflight", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    transfer_root = invocation.root / "transfers"
    transfer_root.mkdir()
    holder: dict[str, Any] = {}

    def upload(message: dict[str, Any], archive_path: str) -> None:
        identity = run_identity(message)
        state = holder["state"]
        if state.active_case is None or not same_run(identity, state.active_case):
            raise ValueError("stale or uncorrelated benchmark attempt upload")
        machine_id = message.get("machine_id", "")
        if (not SAFE_ID.fullmatch(machine_id) or machine_id != state.peer_id or
            not SAFE_CASE_ID.fullmatch(identity["case_id"]) or
            not SAFE_ID.fullmatch(identity["attempt_id"])):
            raise ValueError("upload machine identity does not match the connected agent")
        target = benchmark_attempt_path(
            invocation, suite_id, machine_id, identity["case_id"],
            f"run-{identity['run_index']:03d}", identity["attempt_id"], create=False,
        )
        _extract_upload(archive_path, target)
        peer_result = json.loads((target / "peer-result.json").read_text(encoding="utf-8"))
        if not same_run(peer_result, identity) or peer_result.get("machine_id") != machine_id:
            shutil.rmtree(target)
            raise ValueError("uploaded peer result identity is invalid")
        holder["transfer_log"](
            f"upload accepted machine={machine_id} case={identity['case_id']} "
            f"run={identity['run_index']} attempt={identity['attempt_id']}"
        )

    try:
        run_log = _RunLog(log_path)
        transfer_log = _RunLog(invocation.root / "transfer.log")
        holder["transfer_log"] = transfer_log
        server, state = start_control_server(args.control, suite_id, invocation.invocation_id,
                                              run_log, upload,
                                              upload_temp_dir=transfer_root)
    except Exception as error:
        _log(log_path, traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-control-start", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    holder["state"] = state
    manifest.data.update({"suite_id": suite_id, "machine_id": args.machine_id,
                          "network_profile": args.network_profile,
                          "topology": {"machines": 2, "peers": 2, "engine": "universal-direct-mesh"}})
    manifest.write()
    results = []
    try:
        run_log(f"coordinator ready control={args.control} suite={suite_id} invocation={invocation.invocation_id}")
        transfer_log(f"coordinator transfer channel ready suite={suite_id}")
        if not state.wait_for_peer(args.initial_agent_timeout_s):
            raise TimeoutError("timed out waiting for benchmark agent")
        agent_id = state.snapshot()["peer_id"]
        if agent_id == args.machine_id:
            raise ValueError("coordinator and agent must use distinct --machine-id values")
        host, audio_port_text = args.audio_bind.rsplit(":", 1)
        audio_port = int(audio_port_text)
        if audio_port <= 0: raise ValueError("benchmark coordinator requires a fixed --audio-bind port")
        public_endpoint = f"{args.public_audio_host}:{audio_port}"
        for case in cases:
            for run_index in range(1, case.repeats + 1):
                completed = False
                for attempt_number in range(1, args.case_retry_limit + 2):
                    attempt_id = uuid.uuid4().hex[:12]
                    identity = {"suite_id": suite_id, "case_id": case.case_id,
                                "run_index": run_index, "attempt_id": attempt_id}
                    attempt = benchmark_attempt_path(invocation, suite_id, args.machine_id,
                                                     case.case_id, f"run-{run_index:03d}", attempt_id)
                    session_id = f"{secrets.randbits(64) or 1:016x}"; session_key = secrets.token_hex(16)
                    invite = f"jam2://v1?endpoint={public_endpoint}&session={session_id}&key={session_key}"
                    scenario = _scenario(case, "network.create", f"{normalized_path_id(case.case_id)}-{attempt_id}", attempt,
                                         args.audio_device, args.sample_rate, args.audio_bind, invite,
                                         session_id, session_key, public_endpoint, "coordinator", args.machine_id)
                    scenario_path = attempt / "scenario.json"; write_scenario(scenario_path, scenario)
                    process, stdout, stderr = _start_native(args.jam2, scenario_path, attempt)
                    offer = {"status": "running", **identity, "invocation_id": invocation.invocation_id,
                             "coordinator_machine_id": args.machine_id, "agent_machine_id": agent_id,
                             "invite": invite, "case": case.metadata(), "sample_rate": args.sample_rate,
                             "coordinator_network_profile": args.network_profile}
                    state.publish_case(offer)
                    timeout = args.case_timeout_s or case.stream_ms / 1000.0 + 90.0
                    code, timed_out = _wait_native(process, timeout)
                    stdout.close(); stderr.close()
                    local = _write_peer_result(attempt, identity, args.machine_id, "coordinator", code, timed_out, case)
                    local["network_profile"] = args.network_profile
                    (attempt / "peer-result.json").write_text(
                        json.dumps(local, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                    )
                    uploaded = state.wait_for(lambda: state.has_message("case.uploaded", identity), args.upload_timeout_s)
                    result = {**identity, "attempt_number": attempt_number,
                              "peers": [local], "agent_artifacts_received": bool(uploaded),
                              "control": state.snapshot()}
                    if uploaded:
                        agent_root = benchmark_attempt_path(
                            invocation, suite_id, agent_id, case.case_id,
                            f"run-{run_index:03d}", attempt_id, create=False,
                        )
                        agent_result = json.loads((agent_root / "peer-result.json").read_text(encoding="utf-8"))
                        coordinator_csv = attempt / local["csv_path"] if local["csv_path"] else None
                        agent_csv = agent_root / agent_result["csv_path"] if agent_result["csv_path"] else None
                        result["peers"].append(agent_result)
                        result["metrics"] = normalized_pair_summary(
                            args.machine_id, coordinator_csv, agent_id, agent_csv)
                        result["verdict"], completed = _correlated_process_outcome(
                            code, agent_result)
                    else:
                        result["verdict"] = "upload-timeout"
                    (attempt / "correlated-result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                    results.append(result)
                    manifest.add_case({"id": case.case_id, "run_index": run_index,
                                       "attempt_id": attempt_id, "status": "passed" if completed else "failed",
                                       "verdict": result["verdict"]})
                    state.clear_case()
                    if completed: break
                    run_log(f"retrying {case.case_id} run={run_index} attempt={attempt_number}")
                if not completed: raise RuntimeError(f"benchmark attempt retries exhausted: {case.case_id} run {run_index}")
        state.mark_all_done()
        ack = state.wait_for(lambda: state.done_acked, args.finish_grace_s)
        manifest.finish("passed" if ack else "failed", 0 if ack else 1,
                        all_done_acknowledged=bool(ack), results=results,
                        native_profiles={name: capabilities.profile(name) for name in capabilities.profiles})
        return 0 if ack else 1
    except Exception as error:
        run_log(traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-coordinator", "status": "infrastructure-error", "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2, results=results)
        return 2
    finally:
        run_log.freeze()
        transfer_log.freeze()
        server.shutdown(); server.server_close()
        manifest.refresh_artifacts(); manifest.write()


def _agent(args: Any, repo: Path, arguments: list[str]) -> int:
    try:
        _validate_common_arguments(args)
        if args.audio_device is None and not args.headless_audio:
            raise ValueError("benchmark agent requires --audio-device or --headless-audio")
        if not 0.1 <= args.connect_timeout_s <= 86_500:
            raise ValueError("--connect-timeout-s is outside its bound")
        host, port = parse_host_port(
            args.coordinator if ":" in args.coordinator else f"{args.coordinator}:49000")
        if not host or not 1 <= port <= 65535:
            raise ValueError("--coordinator must identify a host and port from 1 through 65535")
    except Exception as error:
        return _failure_manifest(
            repo, args, arguments, "agent-preflight-failed", "benchmark-agent-preflight",
            f"{type(error).__name__}: {error}", clean=args.clean)
    control = BenchmarkControlClient(host, port, args.machine_id,
                                     lambda value: print(value, flush=True))
    control.start()
    if not control.wait_connected(args.connect_timeout_s):
        control.close()
        failed = allocate_invocation("benchmark", repo / "tools", args.output, "agent-connect-failed", args.clean)
        failed_manifest = InvocationManifest(failed.root / "invocation-manifest.json", "benchmark", failed.invocation_id, arguments)
        failed_manifest.add_case({"id": "agent-connect", "status": "infrastructure-error",
                                  "error": f"timed out connecting to {host}:{port}"})
        failed_manifest.finish("infrastructure-error", 2)
        return 2
    try:
        invocation = adopt_invocation("benchmark", repo / "tools", control.invocation_id, args.output, args.clean)
        manifest = InvocationManifest(invocation.root / "invocation-manifest.json", "benchmark", invocation.invocation_id, arguments)
        log_path = invocation.root / "agent.log"
        run_log = _RunLog(log_path)
        transfer_log = _RunLog(invocation.root / "transfer.log")
        control.log = run_log
        capabilities = NativeCapabilities(args.jam2)
        configure_native_profiles(capabilities.description)
        validate_native_attempt_root(benchmark_attempt_path(
            invocation, control.suite_id, args.machine_id, "x" * 24,
            "run-100", "0123456789ab", create=False,
        ))
    except Exception as error:
        control.close()
        return _failure_manifest(
            repo, args, arguments, "agent-start-failed", "benchmark-agent-start",
            f"{type(error).__name__}: {error}")
    manifest.data.update({"suite_id": control.suite_id, "machine_id": args.machine_id,
                          "coordinator": f"{host}:{port}",
                          "network_profile": args.network_profile}); manifest.write()
    run_log(f"agent ready coordinator={host}:{port} suite={control.suite_id} invocation={invocation.invocation_id}")
    transfer_log(f"agent transfer channel ready suite={control.suite_id}")
    completed: set[tuple[Any, ...]] = set()
    disconnected_since: float | None = None
    try:
        while True:
            message = control.wait_message(1.0)
            if message is None:
                if control.connected_event.is_set():
                    disconnected_since = None
                else:
                    disconnected_since = disconnected_since or time.monotonic()
                    if time.monotonic() - disconnected_since >= args.connect_timeout_s:
                        raise TimeoutError(
                            f"benchmark coordinator was unavailable for {args.connect_timeout_s:g} seconds"
                        )
                continue
            if message.get("type") == "control.disconnected":
                disconnected_since = disconnected_since or time.monotonic()
                if time.monotonic() - disconnected_since >= args.connect_timeout_s:
                    raise TimeoutError(
                        f"benchmark coordinator was unavailable for {args.connect_timeout_s:g} seconds"
                    )
                continue
            disconnected_since = None
            if message.get("type") == "all_done":
                control.send({"type": "done.ack", "suite_id": control.suite_id, "machine_id": args.machine_id})
                manifest.finish("passed", 0, all_done_acknowledged=True)
                return 0
            if message.get("type") != "case.offer": continue
            offer = message.get("case", {})
            identity = run_identity(offer)
            key = tuple(identity.values())
            if key in completed:
                control.send({"type": "case.uploaded", **identity, "machine_id": args.machine_id, "duplicate_offer": True})
                continue
            if offer.get("invocation_id") != invocation.invocation_id or offer.get("agent_machine_id") != args.machine_id:
                continue
            if (identity["suite_id"] != control.suite_id or
                    not SAFE_CASE_ID.fullmatch(identity["case_id"]) or
                    not SAFE_ID.fullmatch(identity["attempt_id"]) or
                    not 1 <= identity["run_index"] <= 100):
                raise ValueError("offered benchmark attempt identity is invalid")
            case_meta = offer["case"]
            if not isinstance(case_meta, dict):
                raise ValueError("offered benchmark case metadata must be an object")
            offered_stream_ms = int(case_meta.get("stream_ms", 0) or 0)
            if not 100 <= offered_stream_ms <= 86_400_000:
                raise ValueError("offered benchmark stream duration is outside its bound")
            # Reconstruct only from the coordinator's bounded retained catalog
            # identity, never from arbitrary offered argv.
            all_cases = benchmark_cases(
                signals=("silence", "tone-440", "pulse-1s"),
                stream_ms=offered_stream_ms, repeats=1)
            selected = next((case for case in all_cases if case.case_id == identity["case_id"]), None)
            if selected is None: raise ValueError(f"agent does not recognize offered case {identity['case_id']}")
            if selected.metadata()["profile"] != case_meta.get("profile"):
                raise ValueError("offered benchmark profile/override metadata does not match the retained catalog")
            attempt = benchmark_attempt_path(invocation, identity["suite_id"], args.machine_id,
                                             identity["case_id"], f"run-{identity['run_index']:03d}", identity["attempt_id"])
            control.send({"type": "case.accept", **identity, "machine_id": args.machine_id})
            scenario = _scenario(selected, "network.join", f"{normalized_path_id(identity['case_id'])}-{identity['attempt_id']}", attempt,
                                 args.audio_device, args.sample_rate, "0.0.0.0:0", offer["invite"],
                                 "", "", "", "agent", args.machine_id)
            scenario_path = attempt / "scenario.json"; write_scenario(scenario_path, scenario)
            process, stdout, stderr = _start_native(args.jam2, scenario_path, attempt)
            control.send({"type": "case.started", **identity, "machine_id": args.machine_id})
            timeout = args.case_timeout_s or selected.stream_ms / 1000.0 + 90.0
            code, timed_out = _wait_native(process, timeout); stdout.close(); stderr.close()
            peer = _write_peer_result(attempt, identity, args.machine_id, "agent", code, timed_out, selected)
            peer["network_profile"] = args.network_profile
            peer["coordinator_network_profile"] = offer.get("coordinator_network_profile", "unknown")
            (attempt / "peer-result.json").write_text(
                json.dumps(peer, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            control.send({"type": "case.finished", **identity, "machine_id": args.machine_id,
                          "return_code": code})
            archive = _zip_attempt(attempt)
            transfer_log(
                f"upload started case={identity['case_id']} run={identity['run_index']} "
                f"attempt={identity['attempt_id']} bytes={archive.stat().st_size}"
            )
            acknowledged = control.upload_artifact({**identity, "machine_id": args.machine_id}, archive, timeout_s=120)
            transfer_log(
                f"upload {'acknowledged' if acknowledged else 'failed'} case={identity['case_id']} "
                f"run={identity['run_index']} attempt={identity['attempt_id']}"
            )
            manifest.add_case({"id": identity["case_id"], "run_index": identity["run_index"],
                               "attempt_id": identity["attempt_id"],
                               "status": _agent_attempt_status(acknowledged, code),
                               "return_code": code, "native_effective_configuration": peer["effective_configuration"]})
            if not acknowledged: raise RuntimeError("coordinator rejected or did not acknowledge artifact upload")
            completed.add(key)
            archive.unlink(missing_ok=True)
            if args.delete_after_upload:
                resolved = attempt.resolve(); root = invocation.root.resolve()
                resolved.relative_to(root)
                shutil.rmtree(attempt)
    except Exception as error:
        run_log(traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-agent", "status": "infrastructure-error", "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    finally:
        control.close()
        run_log.freeze()
        transfer_log.freeze()
        manifest.refresh_artifacts(); manifest.write()


def _analyze(args: Any, repo: Path, arguments: list[str]) -> int:
    invocation = allocate_invocation("benchmark", repo / "tools", args.output, "analyze", args.clean)
    manifest = InvocationManifest(invocation.root / "invocation-manifest.json", "benchmark", invocation.invocation_id, arguments)
    try:
        if not args.results.is_dir():
            raise ValueError("benchmark analysis source must be an existing directory")
        results = []
        total_bytes = 0
        for path in args.results.rglob("correlated-result.json"):
            if len(results) >= MAX_ANALYSIS_RESULTS:
                raise ValueError("benchmark analysis source exceeds its result-file bound")
            total_bytes += path.stat().st_size
            if total_bytes > MAX_ANALYSIS_SOURCE_BYTES:
                raise ValueError("benchmark analysis source exceeds its 1 GiB input bound")
            value = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(value, dict):
                raise ValueError(f"benchmark result is not an object: {path}")
            results.append(value)
        summary = {"source": str(args.results.resolve()), "attempts": len(results),
                   "complete": sum(1 for item in results if item.get("verdict") == "complete"),
                   "results": results}
        (invocation.root / "analysis.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        with (invocation.root / "analysis.csv").open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=("suite_id", "case_id", "run_index", "attempt_id", "verdict"))
            writer.writeheader()
            for item in results: writer.writerow({key: item.get(key, "") for key in writer.fieldnames})
        manifest.add_case({"id": "analyze", "status": "passed", "attempts": len(results)})
        manifest.finish("passed", 0)
        return 0
    except Exception as error:
        manifest.add_case({"id": "analyze", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2


def run(args: Any, repo: Path, arguments: list[str]) -> int:
    if args.benchmark_mode == "coordinator": return _coordinator(args, repo, arguments)
    if args.benchmark_mode == "agent": return _agent(args, repo, arguments)
    return _analyze(args, repo, arguments)
