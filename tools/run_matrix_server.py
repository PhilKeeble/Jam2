#!/usr/bin/env python3

import argparse
import http.server
import json
import subprocess
import threading
from pathlib import Path

from analyze_matrix_csv import analyze_matrix_csv
from collect_matrix_csv import collect_matrix_csv
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


class MatrixHttpHandler(http.server.SimpleHTTPRequestHandler):
    public_dir = None
    logs_dir = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(self.public_dir), **kwargs)

    def log_message(self, format, *args):
        print_flush("[http] " + (format % args))

    def do_POST(self):
        if self.path != "/upload":
            self.send_error(404, "unknown upload endpoint")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "invalid content length")
            return
        if length <= 0:
            self.send_error(400, "empty upload")
            return
        payload = json.loads(self.rfile.read(length).decode("utf-8"))
        test_id = payload["test_id"]
        run_index = int(payload["run_index"])
        side = payload.get("side", "client")
        if side != "client":
            self.send_error(400, "only client uploads are accepted")
            return
        csv_text = payload["csv"]
        output_dir = run_dir(self.logs_dir, test_id, side, run_index)
        ensure_dir(output_dir)
        target = output_dir / "stats.csv"
        with open(target, "w", encoding="utf-8", newline="") as handle:
            handle.write(csv_text)
        write_json(output_dir / "upload.json", {
            "test_id": test_id,
            "run_index": run_index,
            "side": side,
            "bytes": len(csv_text.encode("utf-8")),
        })
        print_flush(f"[server] received client CSV for {test_id} run {run_index}")
        response = b'{"ok":true}\n'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


def start_http_server(public_dir, logs_dir, host, port):
    MatrixHttpHandler.public_dir = public_dir
    MatrixHttpHandler.logs_dir = logs_dir
    handler = MatrixHttpHandler
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
    server = start_http_server(public_dir, base_logs, args_ns.host, args_ns.port)
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
        try:
            combined, written, file_count = collect_matrix_csv(base_logs, side="all")
            print_flush(f"[server] combined {written} row(s) from {file_count} CSV file(s): {combined}")
            analysis, rows = analyze_matrix_csv(combined, print_top=True)
            print_flush(f"[server] wrote analysis for {len(rows)} profile(s): {analysis}")
        except SystemExit as error:
            print_flush(f"[server] analysis skipped: {error}")
        return 0
    finally:
        server.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
