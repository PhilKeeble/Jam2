#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import zipfile
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_benchmark_control import BenchmarkControlClient, control_endpoint_from_server_url
from jam2_harness import rewrite_jam_url_endpoint
from jam2_tooling import copy_final_csv, default_jam2_path, ensure_dir, fail, print_flush, repo_root, write_json


def parse_args():
    parser = argparse.ArgumentParser(description="Run Jam2 static benchmark cases by polling a benchmark server.")
    parser.add_argument("--server", required=True, help="Server host or diagnostic HTTP URL, for example 192.168.1.50")
    parser.add_argument("--control", default="", help="TCP control endpoint; defaults to SERVER host with port 49000")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--client-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("benchmark_logs")))
    parser.add_argument("--poll-ms", type=int, default=500)
    parser.add_argument("--timeout-s", type=int, default=0)
    parser.add_argument("--finish-idle-s", type=float, default=0.0, help="optional fallback exit if the server disappears after completed work; 0 waits for all_done")
    parser.add_argument("--post-upload-pause-s", type=float, default=5.0, help="wait after each upload before polling for the next case")
    parser.add_argument("--case-timeout-s", type=float, default=0.0, help="kill a Jam2 case after this many seconds; 0 derives from stream/wait time")
    parser.add_argument("--use-published-audio-host", action="store_true", help="use the Jam2 URL host exactly as published by the server")
    parser.add_argument("--clean", action="store_true", help="delete local artifacts after successful upload")
    parser.add_argument("--network-profile", choices=("auto", "wired", "wifi", "unknown"), default="auto")
    return parser.parse_args()


def detect_network_type():
    try:
        if sys.platform == "win32":
            command = [
                "powershell", "-NoProfile", "-Command",
                "(Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1 | "
                "Get-NetAdapter).NdisPhysicalMedium",
            ]
            text = subprocess.check_output(command, text=True, timeout=3, stderr=subprocess.DEVNULL).strip().lower()
            if "wireless" in text or "802.11" in text or "wifi" in text:
                return "wifi"
            if text:
                return "wired"
    except Exception:
        pass
    return "unknown"


def server_host_from_url(base_url):
    parsed = urllib.parse.urlparse(base_url)
    if parsed.hostname:
        return parsed.hostname
    return base_url.strip().rsplit(":", 1)[0]


def jam_url_endpoint_port(jam_url):
    parsed = urllib.parse.urlparse(jam_url)
    values = urllib.parse.parse_qs(parsed.query).get("endpoint", [])
    if not values:
        raise ValueError("Jam2 URL does not contain endpoint")
    _, port_text = values[0].rsplit(":", 1)
    return int(port_text)


def jam_url_query_value(jam_url, key):
    parsed = urllib.parse.urlparse(jam_url)
    values = urllib.parse.parse_qs(parsed.query).get(key, [])
    return values[0] if values else ""


def build_jam_url_from_server(current, server_url):
    host = server_host_from_url(server_url)
    if not host:
        return current["url"]
    session_id = current.get("session_id") or jam_url_query_value(current["url"], "session")
    session_key = current.get("session_key") or jam_url_query_value(current["url"], "key")
    audio_port = int(current.get("audio_port") or jam_url_endpoint_port(current["url"]))
    if not session_id or not session_key:
        return rewrite_jam_url_endpoint(current["url"], (host, audio_port))
    return f"jam2://v1?endpoint={host}:{audio_port}&session={session_id}&key={session_key}"


def zip_dir(source_dir, zip_path):
    source_dir = Path(source_dir)
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in source_dir.rglob("*"):
            if path == zip_path or not path.is_file():
                continue
            archive.write(path, path.relative_to(source_dir))


def _copy_stream(pipe, handle):
    try:
        for line in pipe:
            handle.write(line)
            handle.flush()
    finally:
        pipe.close()


def send_control(control, message):
    if control is None:
        return False
    return control.send(message)


def run_case(jam2, current, audio_device, sample_rate, logs, server_url, clean, network_type, case_timeout_s=0.0, use_published_audio_host=False, control=None):
    case_id = current["case_id"]
    run_index = int(current["run_index"])
    suite_id = current.get("suite_id", "")
    attempt_id = current.get("attempt_id", "")
    signal = current["signal"]
    server_signal = current.get("server_signal", signal)
    client_signal = current.get("client_signal", signal)
    output_dir = ensure_dir(Path(logs) / case_id / "client" / f"run_{run_index:02d}")
    csv_dir = ensure_dir(output_dir / "csv_raw")
    recording_dir = ensure_dir(output_dir / "recording")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"
    metadata = {
        "suite_id": suite_id,
        "attempt_id": attempt_id,
        "case_id": case_id,
        "run_index": run_index,
        "side": "client",
        "signal": signal,
        "server_signal": server_signal,
        "client_signal": client_signal,
        "network_type": network_type,
        "profile": current.get("profile", {}),
        "server_args": current.get("server_args", []),
        "client_args": current.get("case_client_args", []),
    }
    write_json(output_dir / "metadata.json", metadata)
    jam_url = current["url"]
    if not use_published_audio_host:
        try:
            rewritten_url = build_jam_url_from_server(current, server_url)
            if rewritten_url != jam_url:
                jam_url = rewritten_url
        except ValueError as error:
            print_flush(f"[client] could not rewrite Jam2 URL host: {error}; using published URL")
    args = [
        str(jam2),
        "connect",
        jam_url,
        "--wait-ms", str(max(15000, int(current.get("stream_ms", 30000)) + 15000)),
        "--audio-device", str(audio_device),
        "--sample-rate", str(sample_rate),
        "--log-stats", str(csv_dir),
        "--record-jam-folder", str(recording_dir),
        "--test-input", client_signal if client_signal != "metronome-only" else "silence",
    ]
    args.extend(current.get("client_args", []))
    timeout_s = case_timeout_s if case_timeout_s and case_timeout_s > 0 else max(30.0, int(current.get("stream_ms", 30000)) / 1000.0 + 30.0)
    print_flush(f"[client] Starting {case_id} run {run_index}")
    send_control(control, {
        "type": "case.started",
        "suite_id": suite_id,
        "case_id": case_id,
        "run_index": run_index,
        "attempt_id": attempt_id,
        "client_time_ms": int(time.time() * 1000),
    })
    with open(stdout_path, "w", encoding="utf-8", newline="") as stdout, open(stderr_path, "w", encoding="utf-8", newline="") as stderr:
        process = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1)
        stdout_thread = threading.Thread(target=_copy_stream, args=(process.stdout, stdout), daemon=True)
        stderr_thread = threading.Thread(target=_copy_stream, args=(process.stderr, stderr), daemon=True)
        stdout_thread.start()
        stderr_thread.start()
        try:
            return_code = process.wait(timeout=timeout_s)
            timed_out = False
        except subprocess.TimeoutExpired:
            print_flush(f"[client] case timeout after {timeout_s:g}s; terminating Jam2")
            timed_out = True
            process.terminate()
            try:
                return_code = process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                return_code = process.wait(timeout=5.0)
        stdout_thread.join(timeout=1.0)
        stderr_thread.join(timeout=1.0)
    copied_csv = copy_final_csv(csv_dir, output_dir)
    analysis = analyze_recording_dir(
        recording_dir,
        signal,
        local_signal=client_signal,
        remote_signal=server_signal)
    write_json(output_dir / "analysis.json", analysis)
    write_json(output_dir / "result.json", {
        **metadata,
        "return_code": return_code,
        "timed_out": timed_out,
        "stats_csv": str(copied_csv) if copied_csv else "",
        "analysis": analysis,
    })
    print_flush(f"[client] Finished {case_id} run {run_index} rc={return_code}")
    send_control(control, {
        "type": "case.finished",
        "suite_id": suite_id,
        "case_id": case_id,
        "run_index": run_index,
        "attempt_id": attempt_id,
        "return_code": return_code,
        "timed_out": timed_out,
        "client_time_ms": int(time.time() * 1000),
    })
    zip_path = output_dir.with_suffix(".zip")
    zip_dir(output_dir, zip_path)
    print_flush(f"[client] Uploading results for {case_id} run {run_index} over TCP control")
    uploaded = control.upload_artifact(metadata, zip_path) if control is not None else False
    if clean and uploaded:
        shutil.rmtree(output_dir, ignore_errors=True)
        try:
            zip_path.unlink()
        except OSError:
            pass
    print_flush(f"[client] Upload complete for {case_id} run {run_index} uploaded={uploaded}")
    return 0 if uploaded else 1


def main():
    args = parse_args()
    jam2 = Path(args.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    logs = ensure_dir(Path(args.logs))
    detected = detect_network_type()
    network_type = detected if args.network_profile == "auto" else args.network_profile
    print_flush(f"[client] network type: {network_type} detected={detected}")
    print_flush(f"[client] connecting to server {args.server}")
    if args.control:
        control_host, control_port_text = args.control.rsplit(":", 1)
        control_port = int(control_port_text)
    else:
        control_host, control_port = control_endpoint_from_server_url(args.server)
    client_id = f"{socket_friendly_host()}-{int(time.time())}"
    control = BenchmarkControlClient(control_host, control_port, client_id, print_flush)
    control.start()
    seen = set()
    completed = set()
    last_completed_at = None
    deadline = time.monotonic() + args.timeout_s if args.timeout_s > 0 else None
    try:
        while True:
            if deadline and time.monotonic() >= deadline:
                return fail("timed out waiting for benchmark server")
            message = control.wait_message(timeout_s=max(0.1, args.poll_ms / 1000.0))
            if message is None:
                if args.finish_idle_s > 0 and completed and last_completed_at and time.monotonic() - last_completed_at >= args.finish_idle_s:
                    print_flush("[client] control idle after completed work")
                    return 0
                continue
            msg_type = message.get("type", "")
            if msg_type == "hello.ok":
                print_flush(f"[client] connection successful suite={message.get('suite_id', '-')}")
                continue
            if msg_type == "control.disconnected":
                print_flush("[client] TCP control disconnected; reconnecting")
                continue
            if msg_type == "all_done":
                send_control(control, {
                    "type": "done.ack",
                    "suite_id": message.get("suite_id", control.suite_id),
                    "client_time_ms": int(time.time() * 1000),
                })
                print_flush("[client] server reports all_done")
                return 0
            if msg_type != "case.offer":
                continue
            current = message.get("case", {})
            if current.get("status") != "running":
                continue
            identity_key = (
                current.get("suite_id", ""),
                current.get("case_id", ""),
                int(current.get("run_index", 0) or 0),
                current.get("attempt_id", ""),
            )
            if identity_key in completed:
                send_control(control, {
                    "type": "case.uploaded",
                    "suite_id": identity_key[0],
                    "case_id": identity_key[1],
                    "run_index": identity_key[2],
                    "attempt_id": identity_key[3],
                    "uploaded": True,
                    "duplicate_offer": True,
                    "client_time_ms": int(time.time() * 1000),
                })
                continue
            if identity_key in seen:
                continue
            seen.add(identity_key)
            send_control(control, {
                "type": "case.accept",
                "suite_id": current.get("suite_id", ""),
                "case_id": current.get("case_id", ""),
                "run_index": int(current.get("run_index", 0) or 0),
                "attempt_id": current.get("attempt_id", ""),
                "client_time_ms": int(time.time() * 1000),
            })
            rc = run_case(
                jam2, current, args.client_audio_device, args.sample_rate,
                logs, args.server, args.clean, network_type, args.case_timeout_s,
                args.use_published_audio_host, control)
            if rc != 0:
                return rc
            completed.add(identity_key)
            last_completed_at = time.monotonic()
            if args.post_upload_pause_s > 0:
                print_flush("[client] Waiting for new case")
                time.sleep(args.post_upload_pause_s)
    finally:
        control.close()


def socket_friendly_host():
    try:
        import socket
        return socket.gethostname().replace(" ", "_")
    except OSError:
        return "client"


if __name__ == "__main__":
    raise SystemExit(main())
