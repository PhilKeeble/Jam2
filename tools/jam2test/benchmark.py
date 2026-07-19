from __future__ import annotations

import csv
import hashlib
import json
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
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
from .benchmark_suite import CATALOG_VERSION, SUITES, benchmark_cases
from .format_comparison import write_format_comparison
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
BENCHMARK_PACKAGE_SCHEMA = "jam2-benchmark-submission-v1"


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


def _validate_common_arguments(args: Any, capabilities: NativeCapabilities) -> None:
    if not SAFE_ID.fullmatch(args.machine_id):
        raise ValueError("--machine-id must use 1..64 letters, numbers, dot, underscore, or hyphen")
    if args.suite not in SUITES:
        raise ValueError("benchmark suite must be core or full")
    capabilities.validate_sparse_overrides({"sample_rate": args.sample_rate})
    if args.case_timeout_s < 0 or args.case_timeout_s > 86_500:
        raise ValueError("--case-timeout-s is outside its bound")
    if args.audio_device is not None:
        capabilities.validate_sparse_overrides({"audio_device": args.audio_device})
    if (args.audio_device is None) == (not args.headless_audio):
        raise ValueError("select exactly one of --audio-device or --headless-audio")


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


def _format_case_validation(result: dict[str, Any], case: Any) -> dict[str, Any]:
    aggregate = result.get("metrics", {}).get("aggregate", {})
    expected = f"{case.network_audio_format}-mono"
    reasons = []
    if aggregate.get("network_audio_formats") != [expected]:
        reasons.append("session format did not match the offered benchmark case")
    if aggregate.get("sent_packets_min", 0.0) <= 0.0 or aggregate.get("recv_packets_min", 0.0) <= 0.0:
        reasons.append("bidirectional network packet flow was not observed")
    if aggregate.get("send_bitrate_bps_max", 0.0) <= 0.0 or aggregate.get("recv_bitrate_bps_max", 0.0) <= 0.0:
        reasons.append("network byte flow was not measured")
    expected_width = 2 if case.network_audio_format == "pcm16" else 3
    if aggregate.get("network_audio_bytes_per_sample_max") != expected_width:
        reasons.append("measured bytes per sample did not match the selected PCM format")
    expected_packet_bytes = 36 + int(aggregate.get("frame_size_max", 0.0)) * expected_width
    if aggregate.get("audio_packet_bytes_max") != expected_packet_bytes:
        reasons.append("measured audio packet size did not match the selected PCM format")
    non_silent_requested = any(
        signal not in ("", "silence", "metronome-only")
        for signal in (case.coordinator_signal, case.agent_signal)
    )
    remote_peaks = [
        float(peer.get("analysis", {}).get("stems", {}).get("their-input", {}).get("peak", 0.0) or 0.0)
        for peer in result.get("peers", [])
    ]
    if non_silent_requested and max(remote_peaks, default=0.0) <= 0.0:
        reasons.append("non-silent remote benchmark audio was not recorded")
    return {
        "ok": not reasons,
        "expected_format": expected,
        "observed_formats": aggregate.get("network_audio_formats", []),
        "expected_audio_packet_bytes": expected_packet_bytes,
        "observed_audio_packet_bytes": aggregate.get("audio_packet_bytes_max", 0.0),
        "non_silent_requested": non_silent_requested,
        "remote_peak_max": max(remote_peaks, default=0.0),
        "reasons": reasons,
    }


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
              audio_device: int | None, sample_rate: int, bind: str, invite: str,
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
        "network_audio_format": case.network_audio_format,
        "stream_ms": 0,
        "test_input": "silence" if signal == "metronome-only" else signal,
    })
    if audio_device is None:
        runtime["headless_audio"] = True
    else:
        runtime["audio_device"] = audio_device
    recording = root / "recording"
    stop_frame = max(sample_rate, round((case.stream_ms / 1000.0 - 1.0) * sample_rate))
    result: dict[str, Any] = {
        "schema": "jam2-debug-scenario", "run_id": run_id,
        "operation": operation, "profile": case.profile.base_profile,
        "runtime": runtime, "artifacts": {"root": str(root)},
        # Static shutdown stops the engine at the requested frame, but the
        # bounded network operation still owns its outer wait. Keep a small
        # connection/teardown margin instead of making every 30-second case
        # idle for another minute.
        "network": {"bind": bind, "no_stun": True, "wait_ms": case.stream_ms + 10000,
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


def _apply_recording_artifact_hashes(
    analysis: dict[str, Any],
    native: dict[str, Any],
) -> None:
    artifacts = {}
    for artifact in native.get("artifacts", []):
        name = Path(str(artifact.get("path", "")).replace("\\", "/")).name
        if name.endswith(".wav"):
            artifacts[name] = artifact
    for stem in analysis.get("stems", {}).values():
        source = artifacts.get(stem.get("path", ""))
        if not source:
            continue
        if "sha256" in source:
            stem["source_sha256"] = source["sha256"]
        if "bytes" in source:
            stem["source_bytes"] = source["bytes"]


def _apply_wav_retention(root: Path, analysis: dict[str, Any]) -> dict[str, Any]:
    recording = root / "recording"
    wavs = [recording / f"{stem}.wav" for stem in (
        "mix", "my-input", "their-input", "inputs-mix", "metronome")]
    retained = [path.name for path in wavs if path.is_file()]
    deleted: list[str] = []
    errors: list[str] = []
    if analysis.get("analysis_complete", False):
        for path in wavs:
            if not path.is_file():
                continue
            try:
                path.unlink()
                deleted.append(path.name)
            except OSError as error:
                errors.append(f"{path.name}: {error}")
        retained = [path.name for path in wavs if path.is_file()]
    if errors:
        reason = "deletion-failed"
    elif analysis.get("analysis_complete", False):
        reason = "analysis-complete"
    else:
        reason = "analysis-incomplete"
    return {
        "analysis_complete": bool(analysis.get("analysis_complete", False)),
        "reason": reason,
        "deleted": deleted,
        "retained": retained,
        "errors": errors,
    }


def _flatten_attempt_artifacts(
    root: Path,
    native: dict[str, Any],
    analysis: dict[str, Any],
) -> dict[str, str]:
    sources = [
        path
        for folder_name in ("csv", "recording")
        for path in (root / folder_name).rglob("*")
        if path.is_file()
    ]
    destinations: dict[Path, Path] = {}
    names: set[str] = set()
    for source in sources:
        destination_name = (
            "stats.csv"
            if source.parent == root / "csv" and source.suffix.lower() == ".csv"
            else source.name)
        destination = root / destination_name
        if destination_name in names or destination.exists():
            raise ValueError(
                f"benchmark artifact flattening would collide at {destination.name}")
        names.add(destination_name)
        destinations[source] = destination
    relative_moves = {
        source.relative_to(root).as_posix(): destination.name
        for source, destination in destinations.items()
    }
    for source, destination in destinations.items():
        source.replace(destination)
    for folder_name in ("csv", "recording"):
        folder = root / folder_name
        if folder.is_dir():
            for child in sorted(
                    (path for path in folder.rglob("*") if path.is_dir()),
                    key=lambda path: len(path.parts), reverse=True):
                child.rmdir()
            folder.rmdir()
    for artifact in native.get("artifacts", []):
        value = str(artifact.get("path", "")).replace("\\", "/")
        if value in relative_moves:
            artifact["path"] = relative_moves[value]
        elif len(Path(value).parts) == 2 and Path(value).parts[0] in {
                "csv", "recording"}:
            # Successfully analyzed WAVs have already been deleted, but their
            # hashes remain useful and should describe the condensed layout.
            artifact["path"] = Path(value).name
    analysis["recording_dir"] = str(root)
    native_path = root / "native-manifest.json"
    if native and native_path.is_file():
        native_path.write_text(
            json.dumps(native, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    return relative_moves


def _coverage_class(peers: list[dict[str, Any]]) -> str:
    modes = [peer.get("audio_mode", "unknown") for peer in peers]
    if modes and all(mode == "headless" for mode in modes):
        return "headless-headless"
    if modes and all(mode == "physical" for mode in modes):
        return "physical-physical"
    if set(modes) <= {"headless", "physical"} and modes:
        return "mixed"
    return "unknown"


def _network_outlier_assessment(
    baseline: dict[str, Any] | None,
    current: dict[str, Any] | None,
) -> dict[str, Any]:
    if not baseline or not current:
        return {
            "available": False,
            "triggered": False,
            "reasons": ["control or case network metrics unavailable"],
        }
    checks = (
        ("rtt_avg_ms_max", 10.0, 0.50, "ms"),
        ("jitter_avg_ms_max", 5.0, 1.00, "ms"),
        ("loss_percent_max", 0.5, 1.00, "percentage-points"),
    )
    reasons = []
    measurements = {}
    for field, floor, ratio, unit in checks:
        base = float(baseline.get(field, 0.0) or 0.0)
        value = float(current.get(field, 0.0) or 0.0)
        delta = abs(value - base)
        threshold = max(floor, abs(base) * ratio)
        measurements[field] = {
            "control": base,
            "case": value,
            "absolute_delta": delta,
            "threshold": threshold,
            "unit": unit,
        }
        if delta > threshold:
            reasons.append(
                f"{field} delta {delta:.6g} exceeded {threshold:.6g} {unit}")
    return {
        "available": True,
        "triggered": bool(reasons),
        "reasons": reasons,
        "measurements": measurements,
    }


def _write_peer_result(root: Path, identity: dict[str, Any], machine_id: str,
                       role: str, return_code: int, timed_out: bool,
                       case: Any) -> dict[str, Any]:
    native_error = ""
    try:
        native = native_manifest(root)
    except (OSError, ValueError) as error:
        native = {}
        native_error = f"{type(error).__name__}: {error}"
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
    _apply_recording_artifact_hashes(analysis, native)
    wav_retention = _apply_wav_retention(root, analysis)
    relative_moves = _flatten_attempt_artifacts(root, native, analysis)
    (root / "recording-analysis.json").write_text(
        json.dumps(analysis, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    native_result = native.get("result", {})
    effective = native.get("effective_configuration", {})
    if effective.get("headless_audio") is True:
        audio_mode = "headless"
    elif effective.get("audio_device") is not None:
        audio_mode = "physical"
    else:
        audio_mode = "unknown"
    performance = {
        key: native_result[key]
        for key in (
            "wall_elapsed_ms",
            "process_cpu_time_ms",
            "process_cpu_percent_one_core",
        )
        if key in native_result
    }
    result = {
        **identity, "machine_id": machine_id, "role": role,
        "local_peer_id": local_peer_id,
        "remote_peer_id": int(csv_summary.get("remote_peer_id", 0) or 0),
        "return_code": return_code, "timed_out": timed_out,
        "native_manifest": "native-manifest.json" if native else "",
        "native_manifest_error": native_error,
        "build": native.get("build", {}),
        "platform": native.get("platform", {}),
        "protocols": native.get("protocols", {}),
        "effective_configuration": effective,
        "audio_mode": audio_mode,
        "csv_path": relative_moves.get(
            csv_path.relative_to(root).as_posix(), csv_path.name)
        if csv_path else "",
        "analysis": analysis,
        "recording_analysis": "recording-analysis.json",
        "wav_retention": wav_retention,
        "performance": performance,
        "case": case.metadata() if hasattr(case, "metadata") else {
            "case_id": identity.get("case_id", ""),
            "signal": case.signal,
        },
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


def _agent_artifact_path(value: str) -> str:
    relative = Path(value.replace("\\", "/"))
    if (relative.is_absolute() or ".." in relative.parts or
            not relative.name or len(relative.parts) != 1):
        raise ValueError(f"unsafe agent artifact path: {value}")
    return f"agent_{relative.name}"


def _rewrite_uploaded_agent_references(target: Path) -> dict[str, Any]:
    peer_path = target / "agent_peer-result.json"
    peer = json.loads(peer_path.read_text(encoding="utf-8"))
    for field in ("csv_path", "native_manifest", "recording_analysis"):
        if peer.get(field):
            peer[field] = _agent_artifact_path(str(peer[field]))
    analysis = peer.get("analysis", {})
    for stem in analysis.get("stems", {}).values():
        if stem.get("path"):
            stem["path"] = f"agent_{Path(str(stem['path'])).name}"
    retention = peer.get("wav_retention", {})
    retention["retained"] = [
        f"agent_{Path(str(name)).name}" for name in retention.get("retained", [])]
    peer_path.write_text(
        json.dumps(peer, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    analysis_path = target / "agent_recording-analysis.json"
    if analysis_path.is_file():
        recording_analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        for stem in recording_analysis.get("stems", {}).values():
            if stem.get("path"):
                stem["path"] = f"agent_{Path(str(stem['path'])).name}"
        analysis_path.write_text(
            json.dumps(recording_analysis, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")

    native_path = target / "agent_native-manifest.json"
    if native_path.is_file():
        native = json.loads(native_path.read_text(encoding="utf-8"))
        for artifact in native.get("artifacts", []):
            if artifact.get("path"):
                artifact["path"] = _agent_artifact_path(str(artifact["path"]))
        native_path.write_text(
            json.dumps(native, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    return peer


def _extract_upload(archive_path: str | Path, target: Path) -> dict[str, Any]:
    total = 0
    destinations: list[tuple[zipfile.ZipInfo, Path]] = []
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
            destination = target / _agent_artifact_path(info.filename)
            if destination.exists() or any(
                    destination == existing for _, existing in destinations):
                raise ValueError(f"agent artifact would collide: {info.filename}")
            destinations.append((info, destination))
        target.mkdir(parents=True, exist_ok=True)
        created: list[Path] = []
        try:
            for info, destination in destinations:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info, "r") as source, destination.open("wb") as output:
                    shutil.copyfileobj(source, output, 64 * 1024)
                created.append(destination)
            return _rewrite_uploaded_agent_references(target)
        except Exception:
            for path in reversed(created):
                path.unlink(missing_ok=True)
            raise


def _selected_cases(args: Any, capabilities: NativeCapabilities) -> list[Any]:
    configure_native_profiles(capabilities.description)
    cases = benchmark_cases(args.suite)
    if not cases: raise ValueError("benchmark selection produced no cases")
    for case in cases:
        capabilities.validate_sparse_overrides(case.profile.overrides)
    return cases


def _coordinator(args: Any, repo: Path, arguments: list[str]) -> int:
    invocation = allocate_invocation("benchmark", repo / "tools", args.output, "coordinator", args.clean)
    manifest = InvocationManifest(invocation.root / "invocation-manifest.json", "benchmark", invocation.invocation_id, arguments)
    log_path = invocation.root / "coordinator.log"
    try:
        capabilities = NativeCapabilities(args.jam2)
        _validate_common_arguments(args, capabilities)
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
        cases = _selected_cases(args, capabilities)
    except Exception as error:
        _log(log_path, traceback.format_exc().rstrip())
        manifest.add_case({"id": "benchmark-coordinator-preflight", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2
    if not args.public_audio_host:
        manifest.finish("infrastructure-error", 2, error="coordinator requires --public-audio-host")
        return 2
    suite_id = new_suite_id()
    try:
        for case in cases:
            validate_native_attempt_root(benchmark_attempt_path(
                invocation, suite_id, "coordinator", case.case_id,
                "100", "0123456789ab", create=False,
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
        artifact_index = int(message.get("artifact_index", 0) or 0)
        if (not 1 <= artifact_index <= 1000 or
                artifact_index != int(state.active_case.get("artifact_index", 0) or 0)):
            raise ValueError("upload execution index does not match the active benchmark attempt")
        machine_id = message.get("machine_id", "")
        if (not SAFE_ID.fullmatch(machine_id) or machine_id != state.peer_id or
            not SAFE_CASE_ID.fullmatch(identity["case_id"]) or
            not SAFE_ID.fullmatch(identity["attempt_id"])):
            raise ValueError("upload machine identity does not match the connected agent")
        target = benchmark_attempt_path(
            invocation, suite_id, "agent", identity["case_id"],
            str(artifact_index), identity["attempt_id"], create=False,
        )
        peer_result = _extract_upload(archive_path, target)
        if not same_run(peer_result, identity) or peer_result.get("machine_id") != machine_id:
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
                          "benchmark_suite": args.suite,
                          "benchmark_catalog_version": CATALOG_VERSION,
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
        control_network_metrics: dict[str, Any] | None = None
        for case in cases:
            artifact_index = 0
            scheduled = [{
                "run_index": 1,
                "run_kind": "primary",
                "confirmation_of_attempt_id": "",
            }]
            while scheduled:
                measurement = scheduled.pop(0)
                run_index = measurement["run_index"]
                run_kind = measurement["run_kind"]
                completed = False
                confirmation_requested = False
                for attempt_number in range(1, args.case_retry_limit + 2):
                    artifact_index += 1
                    attempt_id = uuid.uuid4().hex[:12]
                    identity = {"suite_id": suite_id, "case_id": case.case_id,
                                "run_index": run_index, "attempt_id": attempt_id}
                    artifact_identity = {
                        **identity, "artifact_index": artifact_index}
                    attempt = benchmark_attempt_path(invocation, suite_id, "coordinator",
                                                     case.case_id, str(artifact_index), attempt_id)
                    session_id = f"{secrets.randbits(64) or 1:016x}"; session_key = secrets.token_hex(16)
                    invite = f"jam2://v1?endpoint={public_endpoint}&session={session_id}&key={session_key}"
                    scenario = _scenario(case, "network.create", f"{normalized_path_id(case.case_id)}-{attempt_id}", attempt,
                                         args.audio_device, args.sample_rate, args.audio_bind, invite,
                                         session_id, session_key, public_endpoint, "coordinator", args.machine_id)
                    scenario_path = attempt / "scenario.json"; write_scenario(scenario_path, scenario)
                    process, stdout, stderr = _start_native(args.jam2, scenario_path, attempt)
                    offer = {"status": "running", **artifact_identity,
                             "invocation_id": invocation.invocation_id,
                             "coordinator_machine_id": args.machine_id, "agent_machine_id": agent_id,
                             "invite": invite, "case": case.metadata(), "sample_rate": args.sample_rate,
                             "run_kind": run_kind,
                             "confirmation_of_attempt_id": measurement["confirmation_of_attempt_id"],
                             "benchmark_suite": args.suite,
                             "benchmark_catalog_version": CATALOG_VERSION,
                             "coordinator_network_profile": args.network_profile}
                    state.publish_case(offer)
                    timeout = args.case_timeout_s or case.stream_ms / 1000.0 + 90.0
                    code, timed_out = _wait_native(process, timeout)
                    stdout.close(); stderr.close()
                    local = _write_peer_result(
                        attempt, artifact_identity, args.machine_id,
                        "coordinator", code, timed_out, case)
                    local["network_profile"] = args.network_profile
                    (attempt / "peer-result.json").write_text(
                        json.dumps(local, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                    )
                    uploaded = state.wait_for(lambda: state.has_message("case.uploaded", identity), args.upload_timeout_s)
                    result = {**artifact_identity,
                              "attempt_number": attempt_number,
                              "run_kind": run_kind,
                              "confirmation_of_attempt_id": measurement["confirmation_of_attempt_id"],
                              "benchmark_suite": args.suite,
                              "benchmark_catalog_version": CATALOG_VERSION,
                              "case": case.metadata(),
                              "peers": [local], "agent_artifacts_received": bool(uploaded),
                              "control": state.snapshot()}
                    if uploaded:
                        agent_root = benchmark_attempt_path(
                            invocation, suite_id, "agent", case.case_id,
                            str(artifact_index), attempt_id, create=False,
                        )
                        agent_result = json.loads(
                            (agent_root / "agent_peer-result.json").read_text(
                                encoding="utf-8"))
                        coordinator_csv = attempt / local["csv_path"] if local["csv_path"] else None
                        agent_csv = agent_root / agent_result["csv_path"] if agent_result["csv_path"] else None
                        result["peers"].append(agent_result)
                        result["coverage_class"] = _coverage_class(result["peers"])
                        result["metrics"] = normalized_pair_summary(
                            args.machine_id, coordinator_csv, agent_id, agent_csv)
                        result["verdict"], completed = _correlated_process_outcome(
                            code, agent_result)
                        result["format_validation"] = _format_case_validation(result, case)
                        if completed and not result["format_validation"]["ok"]:
                            result["verdict"] = "format-validation-failed"
                            completed = False
                    else:
                        result["coverage_class"] = _coverage_class(result["peers"])
                        result["verdict"] = "upload-timeout"
                    if completed:
                        aggregate = result.get("metrics", {}).get("aggregate", {})
                        if case.case_id == "control-fast-tone-pcm24" and run_kind == "primary":
                            control_network_metrics = dict(aggregate)
                            result["outlier_assessment"] = {
                                "available": True,
                                "triggered": False,
                                "reasons": [],
                                "control_case": True,
                            }
                        else:
                            result["outlier_assessment"] = _network_outlier_assessment(
                                control_network_metrics, aggregate)
                            if (run_kind == "primary" and
                                    result["outlier_assessment"]["triggered"]):
                                confirmation_requested = True
                    (attempt / "benchmark-result.json").write_text(
                        json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
                    results.append(result)
                    manifest.add_case({"id": case.case_id, "run_index": run_index,
                                       "artifact_index": artifact_index,
                                       "run_kind": run_kind,
                                       "attempt_id": attempt_id, "status": "passed" if completed else "failed",
                                       "verdict": result["verdict"]})
                    state.clear_case()
                    if completed: break
                    run_log(f"retrying {case.case_id} run={run_index} attempt={attempt_number}")
                if not completed: raise RuntimeError(f"benchmark attempt retries exhausted: {case.case_id} run {run_index}")
                if confirmation_requested:
                    scheduled.append({
                        "run_index": 2,
                        "run_kind": "outlier-confirmation",
                        "confirmation_of_attempt_id": result["attempt_id"],
                    })
        format_comparison = write_format_comparison(invocation.root, results)
        analysis = _write_analysis_outputs(invocation.root, results, ".")
        state.mark_all_done()
        ack = state.wait_for(lambda: state.done_acked, args.finish_grace_s)
        manifest.finish("passed" if ack else "failed", 0 if ack else 1,
                        all_done_acknowledged=bool(ack), results=results,
                        benchmark_suite=args.suite,
                        benchmark_catalog_version=CATALOG_VERSION,
                        format_comparison={
                            "json": "format-comparison.json", "csv": "format-comparison.csv",
                            "pair_count": format_comparison["pair_count"]},
                        analysis={
                            "json": "analysis.json", "csv": "analysis.csv",
                            "peer_csv": "peer-analysis.csv",
                            "comparisons_json": "comparisons.json",
                            "comparisons_csv": "comparisons.csv",
                            "attempts": analysis["attempts"],
                        },
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
        try:
            transfer_root.rmdir()
        except OSError:
            pass
        manifest.refresh_artifacts(); manifest.write()


def _agent(args: Any, repo: Path, arguments: list[str]) -> int:
    try:
        capabilities = NativeCapabilities(args.jam2)
        _validate_common_arguments(args, capabilities)
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
        configure_native_profiles(capabilities.description)
        validate_native_attempt_root(benchmark_attempt_path(
            invocation, control.suite_id, "agent", "x" * 64,
            "100", "0123456789ab", create=False,
        ))
    except Exception as error:
        control.close()
        return _failure_manifest(
            repo, args, arguments, "agent-start-failed", "benchmark-agent-start",
            f"{type(error).__name__}: {error}")
    manifest.data.update({"suite_id": control.suite_id, "machine_id": args.machine_id,
                          "coordinator": f"{host}:{port}",
                          "benchmark_suite": args.suite,
                          "benchmark_catalog_version": CATALOG_VERSION,
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
            artifact_index = int(offer.get("artifact_index", 0) or 0)
            if not 1 <= artifact_index <= 1000:
                raise ValueError("offered benchmark execution index is invalid")
            artifact_identity = {**identity, "artifact_index": artifact_index}
            case_meta = offer["case"]
            if not isinstance(case_meta, dict):
                raise ValueError("offered benchmark case metadata must be an object")
            if (offer.get("benchmark_suite") != args.suite or
                    int(offer.get("benchmark_catalog_version", 0) or 0) != CATALOG_VERSION):
                raise ValueError("coordinator benchmark suite or catalog version does not match the agent")
            offered_sample_rate = int(offer.get("sample_rate", 0) or 0)
            if offered_sample_rate != args.sample_rate:
                raise ValueError(
                    f"benchmark sample-rate mismatch: coordinator={offered_sample_rate} "
                    f"agent={args.sample_rate}")
            offered_stream_ms = int(case_meta.get("stream_ms", 0) or 0)
            if not 100 <= offered_stream_ms <= 86_400_000:
                raise ValueError("offered benchmark stream duration is outside its bound")
            # Reconstruct only from the coordinator's bounded retained catalog
            # identity, never from arbitrary offered argv.
            all_cases = benchmark_cases(args.suite)
            selected = next((case for case in all_cases if case.case_id == identity["case_id"]), None)
            if selected is None: raise ValueError(f"agent does not recognize offered case {identity['case_id']}")
            if selected.metadata()["profile"] != case_meta.get("profile"):
                raise ValueError("offered benchmark profile/override metadata does not match the retained catalog")
            attempt = benchmark_attempt_path(invocation, identity["suite_id"], "agent",
                                             identity["case_id"], str(artifact_index),
                                             identity["attempt_id"])
            control.send({"type": "case.accept", **identity, "machine_id": args.machine_id})
            scenario = _scenario(selected, "network.join", f"{normalized_path_id(identity['case_id'])}-{identity['attempt_id']}", attempt,
                                 args.audio_device, args.sample_rate, "0.0.0.0:0", offer["invite"],
                                 "", "", "", "agent", args.machine_id)
            scenario_path = attempt / "scenario.json"; write_scenario(scenario_path, scenario)
            process, stdout, stderr = _start_native(args.jam2, scenario_path, attempt)
            control.send({"type": "case.started", **identity, "machine_id": args.machine_id})
            timeout = args.case_timeout_s or selected.stream_ms / 1000.0 + 90.0
            code, timed_out = _wait_native(process, timeout); stdout.close(); stderr.close()
            peer = _write_peer_result(
                attempt, artifact_identity, args.machine_id,
                "agent", code, timed_out, selected)
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
            acknowledged = control.upload_artifact(
                {**artifact_identity, "machine_id": args.machine_id},
                archive, timeout_s=120)
            transfer_log(
                f"upload {'acknowledged' if acknowledged else 'failed'} case={identity['case_id']} "
                f"run={identity['run_index']} attempt={identity['attempt_id']}"
            )
            manifest.add_case({"id": identity["case_id"], "run_index": identity["run_index"],
                               "artifact_index": artifact_index,
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


def _collect_correlated_results(source: Path) -> list[dict[str, Any]]:
    results = []
    total_bytes = 0
    paths = sorted({
        *source.rglob("benchmark-result.json"),
        *source.rglob("correlated-result.json"),
    })
    for path in paths:
        if len(results) >= MAX_ANALYSIS_RESULTS:
            raise ValueError("benchmark analysis source exceeds its result-file bound")
        total_bytes += path.stat().st_size
        if total_bytes > MAX_ANALYSIS_SOURCE_BYTES:
            raise ValueError("benchmark analysis source exceeds its 1 GiB input bound")
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            raise ValueError(f"benchmark result is not an object: {path}")
        results.append(value)
    return results


def _csv_value(value: Any) -> Any:
    if isinstance(value, (dict, list)):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    return value


def _write_dynamic_csv(path: Path, rows: list[dict[str, Any]], leading: tuple[str, ...]) -> None:
    fields = list(leading)
    for key in sorted({key for row in rows for key in row}):
        if key not in fields:
            fields.append(key)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: _csv_value(row.get(key, "")) for key in fields})


def _analysis_rows(results: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    aggregate_rows = []
    peer_rows = []
    for item in results:
        case = item.get("case", {})
        profile = case.get("profile", {})
        identity = {
            "suite_id": item.get("suite_id", ""),
            "benchmark_suite": item.get("benchmark_suite", ""),
            "catalog_version": item.get("benchmark_catalog_version", ""),
            "case_id": item.get("case_id", ""),
            "category": case.get("category", ""),
            "comparator": case.get("comparator", ""),
            "run_index": item.get("run_index", ""),
            "run_kind": item.get("run_kind", ""),
            "artifact_index": item.get("artifact_index", ""),
            "attempt_id": item.get("attempt_id", ""),
            "attempt_number": item.get("attempt_number", ""),
            "verdict": item.get("verdict", ""),
            "coverage_class": item.get("coverage_class", ""),
            "base_profile": profile.get("base_profile", ""),
            "profile": profile.get("profile", ""),
            "network_audio_format": case.get("network_audio_format", ""),
            "signal": case.get("signal", ""),
            "outlier_triggered": item.get("outlier_assessment", {}).get("triggered", False),
        }
        aggregate = dict(identity)
        aggregate.update({
            f"metric.{key}": value
            for key, value in item.get("metrics", {}).get("aggregate", {}).items()
        })
        aggregate_rows.append(aggregate)
        metric_peers = {
            peer.get("machine_id"): peer.get("metrics", {})
            for peer in item.get("metrics", {}).get("peers", [])
        }
        for peer in item.get("peers", []):
            row = dict(identity)
            row.update({
                "machine_id": peer.get("machine_id", ""),
                "role": peer.get("role", ""),
                "audio_mode": peer.get("audio_mode", ""),
                "audio_analysis_complete": peer.get("analysis", {}).get(
                    "analysis_complete", False),
                "audio_ok": peer.get("analysis", {}).get("ok", False),
                "audio_tags": peer.get("analysis", {}).get("tags", []),
            })
            row.update({
                f"config.{key}": value
                for key, value in peer.get("effective_configuration", {}).items()
            })
            row.update({
                f"performance.{key}": value
                for key, value in peer.get("performance", {}).items()
            })
            row.update({
                f"metric.{key}": value
                for key, value in metric_peers.get(peer.get("machine_id"), {}).items()
            })
            peer_rows.append(row)
    return aggregate_rows, peer_rows


def _comparison_results(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    primary = {
        item.get("case_id"): item
        for item in results
        if item.get("run_kind") == "primary" and item.get("verdict") == "complete"
    }
    comparisons = []
    for case_id, item in primary.items():
        comparator_id = item.get("case", {}).get("comparator", "")
        comparator = primary.get(comparator_id)
        if comparator is None:
            comparisons.append({
                "case_id": case_id,
                "comparator": comparator_id,
                "available": False,
                "deltas": {},
            })
            continue
        values = item.get("metrics", {}).get("aggregate", {})
        baseline = comparator.get("metrics", {}).get("aggregate", {})
        deltas = {}
        for key in sorted(set(values) & set(baseline)):
            left, right = values[key], baseline[key]
            if (isinstance(left, (int, float)) and not isinstance(left, bool) and
                    isinstance(right, (int, float)) and not isinstance(right, bool)):
                deltas[key] = left - right
        comparisons.append({
            "case_id": case_id,
            "comparator": comparator_id,
            "available": True,
            "coverage_class": item.get("coverage_class", ""),
            "deltas": deltas,
        })
    return comparisons


def _write_analysis_outputs(
    root: Path,
    results: list[dict[str, Any]],
    source: str,
) -> dict[str, Any]:
    aggregate_rows, peer_rows = _analysis_rows(results)
    comparisons = _comparison_results(results)
    summary = {
        "schema": "jam2-benchmark-analysis-v1",
        "source": source,
        "attempts": len(results),
        "complete": sum(1 for item in results if item.get("verdict") == "complete"),
        "catalog_versions": sorted({
            int(item.get("benchmark_catalog_version", 0) or 0) for item in results
        }),
        "suites": sorted({
            item.get("benchmark_suite", "") for item in results
            if item.get("benchmark_suite")
        }),
        "coverage_classes": sorted({
            item.get("coverage_class", "") for item in results
            if item.get("coverage_class")
        }),
        "results": results,
    }
    (root / "analysis.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    _write_dynamic_csv(
        root / "analysis.csv", aggregate_rows,
        ("suite_id", "benchmark_suite", "catalog_version", "case_id",
         "run_index", "run_kind", "artifact_index", "attempt_id", "verdict",
         "coverage_class"))
    _write_dynamic_csv(
        root / "peer-analysis.csv", peer_rows,
        ("suite_id", "benchmark_suite", "catalog_version", "case_id",
         "run_index", "run_kind", "artifact_index", "attempt_id", "machine_id",
         "role", "audio_mode", "verdict"))
    (root / "comparisons.json").write_text(
        json.dumps({"comparisons": comparisons}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    comparison_rows = []
    for comparison in comparisons:
        row = {key: value for key, value in comparison.items() if key != "deltas"}
        row.update({
            f"delta.{key}": value
            for key, value in comparison.get("deltas", {}).items()
        })
        comparison_rows.append(row)
    _write_dynamic_csv(
        root / "comparisons.csv", comparison_rows,
        ("case_id", "comparator", "available", "coverage_class"))
    return summary


def _analyze(args: Any, repo: Path, arguments: list[str]) -> int:
    invocation = allocate_invocation("benchmark", repo / "tools", args.output, "analyze", args.clean)
    manifest = InvocationManifest(invocation.root / "invocation-manifest.json", "benchmark", invocation.invocation_id, arguments)
    try:
        if not args.results.is_dir():
            raise ValueError("benchmark analysis source must be an existing directory")
        results = _collect_correlated_results(args.results)
        if not results:
            raise ValueError("benchmark analysis source contains no correlated results")
        summary = _write_analysis_outputs(
            invocation.root, results, str(args.results.resolve()))
        comparison = write_format_comparison(invocation.root, results)
        manifest.add_case({"id": "analyze", "status": "passed", "attempts": len(results)})
        manifest.finish("passed", 0, format_comparison={
            "json": "format-comparison.json",
            "csv": "format-comparison.csv",
            "pair_count": comparison["pair_count"],
        })
        return 0
    except Exception as error:
        manifest.add_case({"id": "analyze", "status": "infrastructure-error",
                           "error": f"{type(error).__name__}: {error}"})
        manifest.finish("infrastructure-error", 2)
        return 2


def _scrub_submission_result(value: dict[str, Any]) -> dict[str, Any]:
    result = json.loads(json.dumps(value))
    result.pop("control", None)
    for peer in result.get("peers", []):
        peer["native_manifest_error"] = (
            "native manifest unavailable" if peer.get("native_manifest_error") else "")
        build = peer.get("build", {})
        build.pop("executable", None)
        peer.get("effective_configuration", {}).pop("log_stats_root", None)
        analysis = peer.get("analysis", {})
        analysis["recording_dir"] = "recording"
        for stem in analysis.get("stems", {}).values():
            if "path" in stem:
                stem["path"] = Path(str(stem["path"])).name
    return result


def _package(args: Any) -> int:
    source = args.results.resolve()
    if not source.is_dir():
        raise ValueError("benchmark package source must be an existing directory")
    results = _collect_correlated_results(source)
    if not results:
        raise ValueError("benchmark package source contains no correlated results")
    safe_results = [_scrub_submission_result(item) for item in results]
    target = (args.output.resolve() if args.output else
              source.parent / f"jam2-benchmark-{source.name}.zip")
    if target.suffix.lower() != ".zip":
        raise ValueError("benchmark package output must end in .zip")
    try:
        target.relative_to(source)
    except ValueError:
        pass
    else:
        raise ValueError("benchmark package output must be outside the source directory")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="jam2-benchmark-package-") as folder:
        staging = Path(folder)
        _write_analysis_outputs(staging, safe_results, ".")
        write_format_comparison(staging, safe_results)
        raw_root = staging / "raw-csv"
        raw_count = 0
        raw_bytes = 0
        for path in source.rglob("*.csv"):
            if (path.name not in {"stats.csv", "agent_stats.csv"} and
                    path.parent.name != "csv"):
                continue
            if raw_count >= MAX_ARCHIVE_FILES:
                raise ValueError("benchmark package contains too many raw CSV files")
            raw_bytes += path.stat().st_size
            if raw_bytes > MAX_ARCHIVE_SOURCE_BYTES:
                raise ValueError("benchmark package raw CSV input exceeds 1 GiB")
            destination = raw_root / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            raw_count += 1
        incomplete_peers: list[tuple[Path, bool]] = []
        for peer_path in source.rglob("*peer-result.json"):
            peer = json.loads(peer_path.read_text(encoding="utf-8"))
            if not peer.get("analysis", {}).get("analysis_complete", False):
                incomplete_peers.append((
                    peer_path.parent, peer_path.name.startswith("agent_")))
        for attempt_root, is_agent in incomplete_peers:
            wavs = [
                *attempt_root.glob("*.wav"),
                *(attempt_root / "recording").glob("*.wav"),
            ]
            for wav in wavs:
                if wav.name.startswith("agent_") != is_agent:
                    continue
                destination = staging / "incomplete-audio" / wav.relative_to(source)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(wav, destination)
        inventory = []
        for path in sorted(staging.rglob("*")):
            if not path.is_file():
                continue
            digest_builder = hashlib.sha256()
            with path.open("rb") as handle:
                for block in iter(lambda: handle.read(1024 * 1024), b""):
                    digest_builder.update(block)
            inventory.append({
                "path": path.relative_to(staging).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": digest_builder.hexdigest(),
            })
        package_manifest = {
            "schema": BENCHMARK_PACKAGE_SCHEMA,
            "source_invocation_id": source.name,
            "catalog_versions": sorted({
                int(item.get("benchmark_catalog_version", 0) or 0)
                for item in safe_results
            }),
            "suites": sorted({
                item.get("benchmark_suite", "") for item in safe_results
                if item.get("benchmark_suite")
            }),
            "coverage_classes": sorted({
                item.get("coverage_class", "") for item in safe_results
                if item.get("coverage_class")
            }),
            "attempts": len(safe_results),
            "incomplete_audio_attempts": len(incomplete_peers),
            "files": inventory,
        }
        (staging / "submission-manifest.json").write_text(
            json.dumps(package_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        temporary = target.with_suffix(target.suffix + ".tmp")
        with zipfile.ZipFile(
                temporary, "w", compression=zipfile.ZIP_DEFLATED,
                allowZip64=True) as archive:
            files = [path for path in staging.rglob("*") if path.is_file()]
            if len(files) > MAX_ARCHIVE_FILES:
                raise ValueError("benchmark submission package contains too many files")
            for path in sorted(files):
                archive.write(path, path.relative_to(staging).as_posix())
        temporary.replace(target)
    print(f"[benchmark] package: {target}", flush=True)
    return 0


def _list_cases(args: Any) -> int:
    capabilities = NativeCapabilities(args.jam2)
    configure_native_profiles(capabilities.description)
    for case in benchmark_cases(args.suite):
        capabilities.validate_sparse_overrides(case.profile.overrides)
        print(
            f"{case.case_id} category={case.category} "
            f"base_profile={case.profile.base_profile} "
            f"format={case.network_audio_format} signal={case.signal} "
            f"comparator={case.comparator} "
            f"overrides={json.dumps(case.profile.overrides, sort_keys=True)} "
            f"purpose={case.purpose}")
    return 0


def run(args: Any, repo: Path, arguments: list[str]) -> int:
    if args.benchmark_mode == "coordinator": return _coordinator(args, repo, arguments)
    if args.benchmark_mode == "agent": return _agent(args, repo, arguments)
    if args.benchmark_mode == "analyze": return _analyze(args, repo, arguments)
    if args.benchmark_mode == "list": return _list_cases(args)
    try:
        return _package(args)
    except Exception as error:
        print(f"[benchmark] package failed: {type(error).__name__}: {error}", flush=True)
        return 2
