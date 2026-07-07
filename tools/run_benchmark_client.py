#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
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
    parser.add_argument("--finish-idle-s", type=float, default=5.0, help="exit cleanly if the server disappears after completed work")
    parser.add_argument("--post-upload-pause-s", type=float, default=5.0, help="wait after each upload before polling for the next case")
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


def run_case(jam2, current, audio_device, sample_rate, logs, server_url, clean, network_type):
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
    }
    write_json(output_dir / "metadata.json", metadata)
    args = [
        str(jam2),
        "connect",
        current["url"],
        "--audio-device", str(audio_device),
        "--sample-rate", str(sample_rate),
        "--log-stats", str(csv_dir),
        "--record-jam-folder", str(recording_dir),
        "--test-input", client_signal if client_signal != "metronome-only" else "silence",
    ]
    args.extend(current.get("client_args", []))
    print_flush(f"[client] starting {case_id} run {run_index} signal={signal}")
    with open(stdout_path, "w", encoding="utf-8", newline="") as stdout, open(stderr_path, "w", encoding="utf-8", newline="") as stderr:
        process = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            stdout=stdout,
            stderr=stderr,
            text=True,
            encoding="utf-8",
            errors="replace")
        return_code = process.wait()
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
    zip_path = output_dir.with_suffix(".zip")
    zip_dir(output_dir, zip_path)
    uploaded = upload_zip(server_url, zip_path)
    if clean and uploaded:
        shutil.rmtree(output_dir, ignore_errors=True)
        try:
            zip_path.unlink()
        except OSError:
            pass
    print_flush(f"[client] finished {case_id} run {run_index} rc={return_code} uploaded={uploaded}")
    return return_code if uploaded else 1


def main():
    args = parse_args()
    jam2 = Path(args.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    logs = ensure_dir(Path(args.logs))
    detected = detect_network_type()
    network_type = detected if args.network_profile == "auto" else args.network_profile
    print_flush(f"[client] network type: {network_type} detected={detected}")
    seen = set()
    completed = set()
    last_completed_at = None
    deadline = time.monotonic() + args.timeout_s if args.timeout_s > 0 else None
    while True:
        if deadline and time.monotonic() >= deadline:
            return fail("timed out waiting for benchmark server")
        try:
            current = fetch_current(args.server)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            if completed and last_completed_at and time.monotonic() - last_completed_at >= args.finish_idle_s:
                print_flush("[client] server stopped after completed work")
                return 0
            print_flush(f"[client] waiting for server: {error}")
            time.sleep(args.poll_ms / 1000.0)
            continue
        if current.get("status") == "all_done":
            print_flush("[client] server reports all_done")
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
            logs, args.server, args.clean, network_type)
        if rc != 0:
            return rc
        completed.add(key)
        last_completed_at = time.monotonic()
        if args.post_upload_pause_s > 0:
            time.sleep(args.post_upload_pause_s)


if __name__ == "__main__":
    raise SystemExit(main())
