#!/usr/bin/env python3

import argparse
import csv
import http.server
import json
import shutil
import socketserver
import threading
import time
import zipfile
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_benchmark_suite import benchmark_cases
from jam2_harness import start_listener
from jam2_metrics import combined_summary
from jam2_tooling import copy_final_csv, default_jam2_path, ensure_dir, fail, print_flush, repo_root, write_json


CLIENT_UPLOAD_TIMEOUT_S = 60


def parse_args():
    parser = argparse.ArgumentParser(description="Run a static recorded Jam2 benchmark suite.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--server-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("benchmark_logs")))
    parser.add_argument("--bind-http", default="0.0.0.0:8000")
    parser.add_argument("--stream-ms", type=int, default=30000)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--signals", default="silence,tone-440,pulse-1s")
    parser.add_argument("--no-metronome-cases", action="store_true")
    parser.add_argument("--list-cases", action="store_true")
    parser.add_argument("--clean", action="store_true")
    return parser.parse_args()


class UploadHandler(http.server.SimpleHTTPRequestHandler):
    public_dir = None
    logs_dir = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(self.public_dir), **kwargs)

    def log_message(self, fmt, *args):
        print_flush("[http] " + (fmt % args))

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
        upload_dir = ensure_dir(Path(self.logs_dir) / "_uploads")
        zip_path = upload_dir / f"client_{int(time.time() * 1000)}.zip"
        zip_path.write_bytes(self.rfile.read(length))
        try:
            with zipfile.ZipFile(zip_path, "r") as archive:
                result = json.loads(archive.read("result.json").decode("utf-8"))
                case_id = result["case_id"]
                run_index = int(result["run_index"])
                target = ensure_dir(Path(self.logs_dir) / case_id / "client" / f"run_{run_index:02d}")
                if target.exists():
                    shutil.rmtree(target)
                ensure_dir(target)
                archive.extractall(target)
        except Exception as error:
            self.send_error(400, f"invalid upload: {error}")
            return
        response = b'{"ok":true}\n'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)
        print_flush(f"[server] received client artifacts for {case_id} run {run_index}")


def serve_http(bind_http, public_dir, logs_dir):
    host, port_text = bind_http.rsplit(":", 1)
    UploadHandler.public_dir = public_dir
    UploadHandler.logs_dir = logs_dir

    class ReusableThreadingTcpServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True

    server = ReusableThreadingTcpServer((host, int(port_text)), UploadHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def wait_for_client_result(logs_dir, case_id, run_index):
    target = Path(logs_dir) / case_id / "client" / f"run_{run_index:02d}" / "result.json"
    deadline = time.monotonic() + CLIENT_UPLOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        if target.exists():
            return target
        time.sleep(0.25)
    return None


def verdict_from_tags(tags):
    fail_tags = {
        "process_failed", "stats_missing", "recording_missing", "recording_writer_errors",
        "stem_length_mismatch", "metronome_click_count_mismatch", "metronome_click_timing_high",
        "my-input_tone_missing", "their-input_tone_missing",
        "listener_startup_failed", "client_upload_missing",
    }
    if any(tag in fail_tags or tag.endswith("_invalid") or tag.endswith("_missing") for tag in tags):
        return "FAIL"
    if tags:
        return "WARN"
    return "PASS"


def evaluate_result(result):
    tags = list(result.get("tags", []))
    if result.get("server_return_code", 0) != 0 or result.get("client_return_code", 0) != 0:
        tags.append("process_failed")
    metrics = result.get("metrics", {}).get("combined", {})
    if not metrics.get("has_csv", False):
        tags.append("stats_missing")
    if metrics.get("loss_percent_max", 0.0) > 0.05:
        tags.append("packet_loss_high")
    if metrics.get("playback_underrun_time_ms_total", 0.0) > 100.0:
        tags.append("underrun_high")
    if metrics.get("playback_dropped_frames_total", 0.0) > 0:
        tags.append("playback_dropped_frames")
    for side in ("server", "client"):
        analysis = result.get(f"{side}_recording_analysis", {})
        tags.extend(analysis.get("tags", []))
    result["tags"] = sorted(set(tags))
    result["verdict"] = verdict_from_tags(result["tags"])
    return result


def write_outputs(logs_dir, results):
    logs_dir = Path(logs_dir)
    write_json(logs_dir / "benchmark_results.json", {"results": results})
    fields = [
        "case_id", "run_index", "profile", "signal", "verdict", "tags",
        "server_return_code", "client_return_code", "loss_percent_max",
        "jitter_max_ms", "rtt_max_ms", "playback_underrun_time_ms_total",
        "playback_dropped_frames_total", "metronome_beat_delta_abs_max",
        "server_csv_path", "client_csv_path",
    ]
    with open(logs_dir / "benchmark_results.csv", "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for result in results:
            combined = result.get("metrics", {}).get("combined", {})
            writer.writerow({
                "case_id": result.get("case_id", ""),
                "run_index": result.get("run_index", ""),
                "profile": result.get("profile", ""),
                "signal": result.get("signal", ""),
                "verdict": result.get("verdict", ""),
                "tags": ";".join(result.get("tags", [])),
                "server_return_code": result.get("server_return_code", ""),
                "client_return_code": result.get("client_return_code", ""),
                "server_csv_path": result.get("server_csv_path", ""),
                "client_csv_path": result.get("client_csv_path", ""),
                **combined,
            })
    lines = []
    lines.append("Jam2 benchmark summary")
    lines.append("")
    lines.append(f"{'case':44} {'profile':28} {'signal':14} {'run':>3} {'verdict':8} tags")
    lines.append("-" * 118)
    for result in results:
        lines.append(
            f"{result.get('case_id','')[:44]:44} "
            f"{result.get('profile','')[:28]:28} "
            f"{result.get('signal','')[:14]:14} "
            f"{result.get('run_index', 0):>3} "
            f"{result.get('verdict',''):8} "
            f"{','.join(result.get('tags', [])) or '-'}")
    (logs_dir / "benchmark_summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_one_case(jam2, audio_device, sample_rate, logs_dir, public_dir, case, run_index):
    output_dir = ensure_dir(Path(logs_dir) / case.case_id / "server" / f"run_{run_index:02d}")
    recording_dir = ensure_dir(output_dir / "recording")
    listener, paths = start_listener(
        jam2,
        audio_device,
        sample_rate,
        case.profile,
        case.stream_ms,
        output_dir,
        extra_args=[
            "--record-jam-folder", str(recording_dir),
            "--test-input", case.signal if case.signal != "metronome-only" else "silence",
        ])
    startup = listener.wait_for_startup("listen", 10.0)
    if not startup or not startup.get("url"):
        listener.terminate()
        return evaluate_result({
            "case_id": case.case_id,
            "run_index": run_index,
            "profile": case.profile.name,
            "signal": case.signal,
            "server_return_code": listener.poll(),
            "client_return_code": -1,
            "tags": ["listener_startup_failed"],
        })
    current = {
        "status": "running",
        "case_id": case.case_id,
        "run_index": run_index,
        "url": startup["url"],
        "signal": case.signal,
        "profile": case.profile.metadata(),
        "client_args": case.profile.args(case.stream_ms),
    }
    write_json(Path(public_dir) / "current.json", current)
    print_flush(f"[server] published {case.case_id} run {run_index}")
    client_result_path = wait_for_client_result(logs_dir, case.case_id, run_index)
    server_rc = listener.wait(timeout=max(1.0, case.stream_ms / 1000.0 + 10.0))
    if server_rc is None:
        listener.terminate()
        server_rc = listener.poll()
    server_csv = copy_final_csv(paths["csv_raw"], paths["dir"])
    server_analysis = analyze_recording_dir(recording_dir, case.signal)
    write_json(output_dir / "analysis.json", server_analysis)
    client_result = {}
    client_csv = None
    client_analysis = {}
    client_rc = -1
    if client_result_path:
        client_result = json.loads(Path(client_result_path).read_text(encoding="utf-8"))
        client_rc = int(client_result.get("return_code", -1))
        client_csv = Path(client_result_path).parent / "stats.csv"
        client_analysis = client_result.get("analysis", {})
    metrics = combined_summary(server_csv, client_csv)
    result = {
        "case_id": case.case_id,
        "run_index": run_index,
        "profile": case.profile.name,
        "profile_values": case.profile.metadata(),
        "signal": case.signal,
        "server_return_code": server_rc,
        "client_return_code": client_rc,
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv and client_csv.exists() else "",
        "metrics": metrics,
        "server_recording_analysis": server_analysis,
        "client_recording_analysis": client_analysis,
        "client_artifacts_received": client_result_path is not None,
    }
    if not client_result_path:
        result["tags"] = ["client_upload_missing"]
    result = evaluate_result(result)
    write_json(output_dir / "result.json", result)
    print_flush(f"[server] {case.case_id} run {run_index}: {result['verdict']} {','.join(result['tags']) or '-'}")
    return result


def main():
    args = parse_args()
    jam2 = Path(args.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    signals = tuple(item.strip() for item in args.signals.split(",") if item.strip())
    cases = benchmark_cases(
        signals=signals,
        include_metronome=not args.no_metronome_cases,
        stream_ms=args.stream_ms,
        repeats=args.repeats)
    if args.list_cases:
        for case in cases:
            print(f"{case.case_id} profile={case.profile.name} signal={case.signal} repeats={case.repeats}")
        return 0
    logs_dir = Path(args.logs)
    if args.clean and logs_dir.exists():
        shutil.rmtree(logs_dir)
    logs_dir = ensure_dir(logs_dir)
    public_dir = ensure_dir(logs_dir / "public")
    server = serve_http(args.bind_http, public_dir, logs_dir)
    results = []
    try:
        for case in cases:
            for run_index in range(1, case.repeats + 1):
                results.append(run_one_case(
                    jam2, args.server_audio_device, args.sample_rate,
                    logs_dir, public_dir, case, run_index))
                write_outputs(logs_dir, results)
        write_json(public_dir / "current.json", {"status": "all_done"})
        write_outputs(logs_dir, results)
    finally:
        server.shutdown()
        server.server_close()
    print_flush(f"[server] wrote {logs_dir / 'benchmark_summary.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
