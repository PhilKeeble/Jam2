#!/usr/bin/env python3

import argparse
import functools
import http.server
import subprocess
import threading
from pathlib import Path

from matrix_common import (
    copy_final_csv,
    default_jam2_path,
    ensure_dir,
    fail,
    load_matrix,
    print_flush,
    repo_root,
    run_dir,
    test_args,
    write_json,
)


def parse_args():
    parser = argparse.ArgumentParser(description="Run Jam2 listen-side test matrix and publish current URL over HTTP.")
    parser.add_argument("--matrix", default=str(Path(__file__).with_name("test_matrix.json")))
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--server-audio-device", required=True)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("logs")))
    parser.add_argument("--no-stun", action="store_true")
    return parser.parse_args()


def start_http_server(public_dir, host, port):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(public_dir))
    server = http.server.ThreadingHTTPServer((host, port), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def run_listen(jam2, matrix, test, run_index, base_logs, public_dir, audio_device, no_stun):
    test_id = test["id"]
    output_dir = run_dir(base_logs, test_id, "server", run_index)
    csv_dir = ensure_dir(output_dir / "csv_raw")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"

    args = [
        str(jam2),
        "listen",
        "--audio-device",
        str(audio_device),
        "--log-stats",
        str(csv_dir),
    ]
    if no_stun:
        args.append("--no-stun")
    args.extend(test_args(matrix, test, "server"))

    public_current = public_dir / "current.json"
    write_json(public_current, {
        "status": "starting",
        "test_id": test_id,
        "run_index": run_index,
        "url": "",
        "client_args": test_args(matrix, test, "client"),
    })

    print_flush(f"[server] starting {test_id} run {run_index}")
    with open(stdout_path, "w", encoding="utf-8", newline="") as stdout, open(stderr_path, "w", encoding="utf-8", newline="") as stderr:
        process = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            stdout=subprocess.PIPE,
            stderr=stderr,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        url = ""
        assert process.stdout is not None
        for line in process.stdout:
            stdout.write(line)
            stdout.flush()
            if not url and "jam2://" in line:
                url = line.strip()
                write_json(public_current, {
                    "status": "running",
                    "test_id": test_id,
                    "run_index": run_index,
                    "url": url,
                    "client_args": test_args(matrix, test, "client"),
                })
                print_flush(f"[server] published URL for {test_id} run {run_index}")
        return_code = process.wait()

    copied_csv = copy_final_csv(csv_dir, output_dir)
    write_json(public_current, {
        "status": "done",
        "test_id": test_id,
        "run_index": run_index,
        "url": "",
        "client_args": [],
        "return_code": return_code,
    })
    print_flush(f"[server] finished {test_id} run {run_index} rc={return_code} csv={copied_csv or 'none'}")
    return return_code


def main():
    args_ns = parse_args()
    matrix = load_matrix(args_ns.matrix)
    jam2 = Path(args_ns.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    base_logs = ensure_dir(Path(args_ns.logs))
    public_dir = ensure_dir(base_logs / "public")
    server = start_http_server(public_dir, args_ns.host, args_ns.port)
    print_flush(f"[server] publishing {public_dir} on http://{args_ns.host}:{args_ns.port}/")
    try:
        for run_index in range(1, args_ns.runs + 1):
            for test in matrix["tests"]:
                rc = run_listen(
                    jam2,
                    matrix,
                    test,
                    run_index,
                    base_logs,
                    public_dir,
                    args_ns.server_audio_device,
                    args_ns.no_stun)
                if rc != 0:
                    return rc
        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
        return 0
    finally:
        server.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
