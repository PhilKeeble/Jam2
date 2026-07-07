#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_harness import rewrite_jam_url_endpoint
from jam2_tooling import copy_final_csv, default_jam2_path, ensure_dir, fail, print_flush, repo_root, write_json


def parse_args():
    parser = argparse.ArgumentParser(description="Run Jam2 static benchmark cases by polling a benchmark server.")
    parser.add_argument("--server", required=True, help="Base URL, for example http://192.168.1.50:8000")
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


def fetch_current(base_url):
    with urllib.request.urlopen(base_url.rstrip("/") + "/current.json", timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def server_host_from_url(base_url):
    parsed = urllib.parse.urlparse(base_url)
    return parsed.hostname or ""


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


def upload_zip(base_url, zip_path):
    data = Path(zip_path).read_bytes()
    request = urllib.request.Request(
        base_url.rstrip("/") + "/upload",
        data=data,
        headers={"Content-Type": "application/zip"},
        method="POST")
    for attempt in range(1, 6):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                if 200 <= response.status < 300:
                    return True
        except urllib.error.URLError as error:
            print_flush(f"[client] upload attempt {attempt} failed: {error}")
        time.sleep(1.0)
    return False


def post_done_ack(base_url):
    request = urllib.request.Request(
        base_url.rstrip("/") + "/done-ack",
        data=b'{"ok":true}\n',
        headers={"Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return 200 <= response.status < 300
    except urllib.error.URLError as error:
        print_flush(f"[client] all_done ack failed: {error}")
        return False


def _copy_stream(pipe, handle):
    try:
        for line in pipe:
            handle.write(line)
            handle.flush()
    finally:
        pipe.close()


def run_case(jam2, current, audio_device, sample_rate, logs, server_url, clean, network_type, case_timeout_s=0.0, use_published_audio_host=False):
    case_id = current["case_id"]
    run_index = int(current["run_index"])
    signal = current["signal"]
    server_signal = current.get("server_signal", signal)
    client_signal = current.get("client_signal", signal)
    output_dir = ensure_dir(Path(logs) / case_id / "client" / f"run_{run_index:02d}")
    csv_dir = ensure_dir(output_dir / "csv_raw")
    recording_dir = ensure_dir(output_dir / "recording")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"
    metadata = {
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
        except subprocess.TimeoutExpired:
            print_flush(f"[client] case timeout after {timeout_s:g}s; terminating Jam2")
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
        "stats_csv": str(copied_csv) if copied_csv else "",
        "analysis": analysis,
    })
    print_flush(f"[client] Finished {case_id} run {run_index} rc={return_code}")
    zip_path = output_dir.with_suffix(".zip")
    zip_dir(output_dir, zip_path)
    print_flush(f"[client] Uploading results for {case_id} run {run_index}")
    uploaded = upload_zip(server_url, zip_path)
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
    seen = set()
    completed = set()
    last_completed_at = None
    connected_to_server = False
    deadline = time.monotonic() + args.timeout_s if args.timeout_s > 0 else None
    while True:
        if deadline and time.monotonic() >= deadline:
            return fail("timed out waiting for benchmark server")
        try:
            current = fetch_current(args.server)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            if args.finish_idle_s > 0 and completed and last_completed_at and time.monotonic() - last_completed_at >= args.finish_idle_s:
                print_flush("[client] server stopped after completed work")
                return 0
            print_flush(f"[client] waiting for server: {error}")
            time.sleep(args.poll_ms / 1000.0)
            continue
        if not connected_to_server:
            print_flush("[client] connection successful")
            connected_to_server = True
        if current.get("status") == "all_done":
            acked = post_done_ack(args.server)
            print_flush(f"[client] server reports all_done acked={acked}")
            return 0
        if current.get("status") != "running":
            time.sleep(args.poll_ms / 1000.0)
            continue
        key = (current.get("case_id"), int(current.get("run_index", 0)))
        if key in seen:
            time.sleep(args.poll_ms / 1000.0)
            continue
        seen.add(key)
        rc = run_case(
            jam2, current, args.client_audio_device, args.sample_rate,
            logs, args.server, args.clean, network_type, args.case_timeout_s,
            args.use_published_audio_host)
        if rc != 0:
            return rc
        completed.add(key)
        last_completed_at = time.monotonic()
        if args.post_upload_pause_s > 0:
            print_flush("[client] Waiting for new case")
            time.sleep(args.post_upload_pause_s)


if __name__ == "__main__":
    raise SystemExit(main())
