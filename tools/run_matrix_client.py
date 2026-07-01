#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from matrix_common import (
    copy_final_csv,
    default_jam2_path,
    ensure_dir,
    fail,
    print_flush,
    repo_root,
    run_dir,
)


def parse_args():
    parser = argparse.ArgumentParser(description="Run Jam2 connect-side test matrix by polling a matrix server.")
    parser.add_argument("--server", required=True, help="Base URL, for example http://192.168.1.50:8000")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--client-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("logs")))
    parser.add_argument("--poll-ms", type=int, default=500)
    parser.add_argument("--timeout-s", type=int, default=0, help="0 waits forever")
    parser.add_argument(
        "--server-gone-grace-s",
        type=int,
        default=10,
        help="exit cleanly if the server is unreachable for this long after at least one completed run")
    parser.add_argument("--clean", action="store_true", help="delete local logs before running and remove uploaded run logs")
    parser.add_argument(
        "--network-profile",
        choices=("auto", "wired", "wifi", "unknown"),
        default="auto",
        help="client network type metadata; auto uses best-effort local detection")
    return parser.parse_args()


def fetch_current(base_url):
    url = base_url.rstrip("/") + "/current.json"
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def read_text_if_exists(path):
    if path is None or not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def detect_network_type():
    try:
        if sys.platform == "win32":
            command = [
                "powershell",
                "-NoProfile",
                "-Command",
                "(Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1 | "
                "Get-NetAdapter).NdisPhysicalMedium"
            ]
            text = subprocess.check_output(command, text=True, timeout=3, stderr=subprocess.DEVNULL).strip().lower()
            if "wireless" in text or "802.11" in text or "wifi" in text:
                return "wifi"
            if text:
                return "wired"
        elif sys.platform == "darwin":
            route = subprocess.check_output(["route", "get", "default"], text=True, timeout=3, stderr=subprocess.DEVNULL)
            iface = ""
            for line in route.splitlines():
                if "interface:" in line:
                    iface = line.split(":", 1)[1].strip()
                    break
            ports = subprocess.check_output(["networksetup", "-listallhardwareports"], text=True, timeout=3, stderr=subprocess.DEVNULL)
            for block in ports.split("\n\n"):
                if f"Device: {iface}" in block:
                    lower = block.lower()
                    if "wi-fi" in lower or "airport" in lower:
                        return "wifi"
                    return "wired"
    except Exception:
        pass
    return "unknown"


def upload_artifacts(base_url, test_id, run_index, csv_path, stdout_path, stderr_path, network_type):
    if csv_path is None or not csv_path.exists():
        print_flush(f"[client] no CSV to upload for {test_id} run {run_index}")
        return False
    payload = json.dumps({
        "test_id": test_id,
        "run_index": run_index,
        "side": "client",
        "csv": read_text_if_exists(csv_path),
        "stdout": read_text_if_exists(stdout_path),
        "stderr": read_text_if_exists(stderr_path),
        "network_type": network_type,
    }).encode("utf-8")
    request = urllib.request.Request(
        base_url.rstrip("/") + "/upload",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST")
    for attempt in range(1, 6):
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                ok = 200 <= response.status < 300
                if ok:
                    print_flush(f"[client] uploaded artifacts for {test_id} run {run_index}")
                    return True
        except urllib.error.URLError as error:
            print_flush(f"[client] upload attempt {attempt} failed: {error}")
        time.sleep(1.0)
    return False


def run_connect(jam2, current, audio_device, sample_rate, base_logs, server_url, clean_after_upload, network_type):
    test_id = current["test_id"]
    run_index = int(current["run_index"])
    output_dir = run_dir(base_logs, test_id, "client", run_index)
    csv_dir = ensure_dir(output_dir / "csv_raw")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"

    args = [
        str(jam2),
        "connect",
        current["url"],
        "--audio-device",
        str(audio_device),
        "--sample-rate",
        str(sample_rate),
        "--log-stats",
        str(csv_dir),
    ]
    args.extend(current.get("client_args", []))

    print_flush(f"[client] starting {test_id} run {run_index}")
    with open(stdout_path, "w", encoding="utf-8", newline="") as stdout, open(stderr_path, "w", encoding="utf-8", newline="") as stderr:
        process = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            stdout=stdout,
            stderr=stderr,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return_code = process.wait()

    copied_csv = copy_final_csv(csv_dir, output_dir)
    uploaded = upload_artifacts(server_url, test_id, run_index, copied_csv, stdout_path, stderr_path, network_type)
    if clean_after_upload and uploaded:
        shutil.rmtree(output_dir)
        print_flush(f"[client] removed uploaded local logs for {test_id} run {run_index}")
    print_flush(f"[client] finished {test_id} run {run_index} rc={return_code} csv={copied_csv or 'none'}")
    return return_code


def main():
    args_ns = parse_args()
    jam2 = Path(args_ns.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    base_logs_path = Path(args_ns.logs)
    if args_ns.clean and base_logs_path.exists():
        shutil.rmtree(base_logs_path)
    base_logs = ensure_dir(base_logs_path)
    detected_network = detect_network_type()
    network_type = detected_network if args_ns.network_profile == "auto" else args_ns.network_profile
    print_flush(f"[client] network type: {network_type} detected={detected_network}")
    seen = set()
    deadline = time.monotonic() + args_ns.timeout_s if args_ns.timeout_s > 0 else None
    server_gone_since = None

    while True:
        if deadline is not None and time.monotonic() >= deadline:
            return fail("timed out waiting for matrix server")
        try:
            current = fetch_current(args_ns.server)
            server_gone_since = None
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            if seen:
                if server_gone_since is None:
                    server_gone_since = time.monotonic()
                elif time.monotonic() - server_gone_since >= args_ns.server_gone_grace_s:
                    print_flush("[client] server stopped after completed runs")
                    return 0
            print_flush(f"[client] waiting for server: {error}")
            time.sleep(args_ns.poll_ms / 1000.0)
            continue

        status = current.get("status")
        if status == "all_done":
            print_flush("[client] server reports all_done")
            return 0
        if status != "running" or not current.get("url"):
            time.sleep(args_ns.poll_ms / 1000.0)
            continue

        key = (current.get("test_id"), int(current.get("run_index", 0)))
        if key in seen:
            time.sleep(args_ns.poll_ms / 1000.0)
            continue
        seen.add(key)
        rc = run_connect(
            jam2,
            current,
            args_ns.client_audio_device,
            args_ns.sample_rate,
            base_logs,
            args_ns.server,
            args_ns.clean,
            network_type)
        if rc != 0:
            return rc


if __name__ == "__main__":
    raise SystemExit(main())
