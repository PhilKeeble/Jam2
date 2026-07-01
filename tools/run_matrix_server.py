#!/usr/bin/env python3

import argparse
import csv
import http.server
import json
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from analyze_matrix_csv import analyze_matrix_csv
from collect_matrix_csv import collect_matrix_csv
from matrix_common import (
    copy_final_csv,
    default_jam2_path,
    ensure_dir,
    fail,
    print_flush,
    repo_root,
    run_dir,
    write_json,
)


PREFILL_VALUES = [512, 768, 1024, 1536, 2048]
FRAME_VALUES = [32, 64, 128, 256]
AUDIO_BUFFER_VALUES = [32, 64, 128, 256, 512]
RUNS_PER_CANDIDATE = 3
SHORT_STREAM_MS = 30000
CONFIRM_STREAM_MS = 60000
STATS_INTERVAL_MS = 5000
STATS_WARMUP_MS = 3000
CLIENT_UPLOAD_TIMEOUT_S = 30


@dataclass(frozen=True)
class Candidate:
    audio_buffer_size: int
    frame_size: int
    playback_prefill_frames: int
    playback_ring_frames: int = 4096
    playback_max_frames: int = 2048
    drift_correction: str = "on"
    drift_deadband_ppm: int = 25

    @property
    def id(self):
        drift = "drift_off" if self.drift_correction == "off" else f"drift_db{self.drift_deadband_ppm}"
        return (
            f"adaptive_audio{self.audio_buffer_size}_frame{self.frame_size}_"
            f"prefill{self.playback_prefill_frames}_{drift}"
        )

    def args(self, stream_ms):
        return [
            "--audio-buffer-size", str(self.audio_buffer_size),
            "--frame-size", str(self.frame_size),
            "--playback-prefill-frames", str(self.playback_prefill_frames),
            "--playback-ring-frames", str(self.playback_ring_frames),
            "--playback-max-frames", str(self.playback_max_frames),
            "--drift-correction", self.drift_correction,
            "--drift-deadband-ppm", str(self.drift_deadband_ppm),
            "--stats-warmup-ms", str(STATS_WARMUP_MS),
            "--stats-interval-ms", str(STATS_INTERVAL_MS),
            "--wait-ms", "30000",
            "--stream-ms", str(stream_ms),
            "--stream-linger-ms", "500",
        ]


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
        output_dir = run_dir(self.logs_dir, test_id, side, run_index)
        ensure_dir(output_dir)
        (output_dir / "stats.csv").write_text(payload.get("csv", ""), encoding="utf-8")
        (output_dir / "stdout.txt").write_text(payload.get("stdout", ""), encoding="utf-8")
        (output_dir / "stderr.txt").write_text(payload.get("stderr", ""), encoding="utf-8")
        write_json(output_dir / "upload.json", {
            "test_id": test_id,
            "run_index": run_index,
            "side": side,
            "bytes": len(payload.get("csv", "").encode("utf-8")),
        })
        print_flush(f"[server] received client artifacts for {test_id} run {run_index}")
        response = b'{"ok":true}\n'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


def parse_args():
    parser = argparse.ArgumentParser(description="Adaptively tune Jam2 settings between two hosts.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--server-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("logs")))
    parser.add_argument("--no-stun", action="store_true")
    parser.add_argument("--clean", action="store_true", help="delete the logs directory before running")
    return parser.parse_args()


def start_http_server(public_dir, logs_dir, host, port):
    MatrixHttpHandler.public_dir = public_dir
    MatrixHttpHandler.logs_dir = logs_dir
    server = http.server.ThreadingHTTPServer((host, port), MatrixHttpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def candidate_ladder():
    for audio_buffer in AUDIO_BUFFER_VALUES:
        for frame_size in FRAME_VALUES:
            for prefill in PREFILL_VALUES:
                yield Candidate(audio_buffer, frame_size, prefill)


def drift_variants(candidate):
    yield candidate
    yield Candidate(
        candidate.audio_buffer_size,
        candidate.frame_size,
        candidate.playback_prefill_frames,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        "on",
        75)
    yield Candidate(
        candidate.audio_buffer_size,
        candidate.frame_size,
        candidate.playback_prefill_frames,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        "off",
        candidate.drift_deadband_ppm)


def numeric(row, field):
    try:
        return float(row.get(field, "") or 0)
    except ValueError:
        return 0.0


def read_csv_rows(path):
    if not path.exists():
        return []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def final_row(rows):
    finals = [row for row in rows if row.get("row_type") == "final"]
    return finals[-1] if finals else {}


def periodic_rows(rows):
    return [row for row in rows if row.get("row_type") == "periodic"]


def side_result(base_logs, candidate_id, side, run_index):
    path = run_dir(base_logs, candidate_id, side, run_index) / "stats.csv"
    rows = read_csv_rows(path)
    return {
        "path": str(path),
        "rows": rows,
        "final": final_row(rows),
        "periodic": periodic_rows(rows),
    }


def wait_for_client_upload(base_logs, candidate_id, run_index):
    target = run_dir(base_logs, candidate_id, "client", run_index) / "stats.csv"
    deadline = time.monotonic() + CLIENT_UPLOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        if target.exists():
            return True
        time.sleep(0.25)
    return False


def run_listen(jam2, candidate, run_index, stream_ms, base_logs, public_dir, audio_device, sample_rate, no_stun):
    output_dir = run_dir(base_logs, candidate.id, "server", run_index)
    csv_dir = ensure_dir(output_dir / "csv_raw")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"
    candidate_args = candidate.args(stream_ms)

    args = [
        str(jam2),
        "listen",
        "--audio-device", str(audio_device),
        "--sample-rate", str(sample_rate),
        "--log-stats", str(csv_dir),
    ]
    if no_stun:
        args.append("--no-stun")
    args.extend(candidate_args)

    current_path = public_dir / "current.json"
    write_json(current_path, {
        "status": "starting",
        "test_id": candidate.id,
        "run_index": run_index,
        "url": "",
        "client_args": candidate_args,
    })

    print_flush(f"[server] starting {candidate.id} run {run_index} stream_ms={stream_ms}")
    with open(stdout_path, "w", encoding="utf-8", newline="") as stdout, open(stderr_path, "w", encoding="utf-8", newline="") as stderr:
        process = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            stdout=subprocess.PIPE,
            stderr=stderr,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1)
        url = ""
        assert process.stdout is not None
        for line in process.stdout:
            stdout.write(line)
            stdout.flush()
            if not url and "jam2://" in line:
                url = line.strip()
                write_json(current_path, {
                    "status": "running",
                    "test_id": candidate.id,
                    "run_index": run_index,
                    "url": url,
                    "client_args": candidate_args,
                })
                print_flush(f"[server] published URL for {candidate.id} run {run_index}")
        return_code = process.wait()

    copied_csv = copy_final_csv(csv_dir, output_dir)
    write_json(public_dir / "current.json", {
        "status": "evaluating",
        "test_id": candidate.id,
        "run_index": run_index,
        "url": "",
        "client_args": [],
        "return_code": return_code,
    })
    print_flush(f"[server] finished {candidate.id} run {run_index} rc={return_code} csv={copied_csv or 'none'}")
    return return_code


def repeating_capture_underruns(rows):
    periodic = periodic_rows(rows)
    if len(periodic) < 3:
        return False
    previous = numeric(periodic[0], "capture_ring_underruns")
    increases = 0
    for row in periodic[1:]:
        current = numeric(row, "capture_ring_underruns")
        if current > previous:
            increases += 1
        previous = current
    return increases >= 2


def evaluate_run(server, client, server_rc, client_uploaded):
    reasons = []
    if server_rc != 0:
        reasons.append(f"server_rc={server_rc}")
    if not client_uploaded:
        reasons.append("client_upload_missing")

    finals = [server["final"], client["final"]]
    for label, row in (("server", server["final"]), ("client", client["final"])):
        if not row:
            reasons.append(f"{label}_final_missing")
            continue
        if numeric(row, "sequence_lost") > 0 or numeric(row, "reordered_lost") > 0:
            reasons.append(f"{label}_packet_loss")
        if numeric(row, "playback_ring_underruns") > 0:
            reasons.append(f"{label}_playback_underrun")
        if numeric(row, "playback_ring_overruns") > 0:
            reasons.append(f"{label}_playback_overrun")
        if numeric(row, "playback_dropped_frames") > 0:
            reasons.append(f"{label}_playback_dropped")
        if numeric(row, "playback_depth_min_ms") < 5.0:
            reasons.append(f"{label}_playback_depth_low")
        if numeric(row, "jitter_max_ms") > 12.0:
            reasons.append(f"{label}_jitter_spike")
    if repeating_capture_underruns(server["rows"]):
        reasons.append("server_repeating_capture_underruns")
    if repeating_capture_underruns(client["rows"]):
        reasons.append("client_repeating_capture_underruns")

    stable = not reasons
    latency_values = [numeric(row, "estimated_one_way_ms") for row in finals if row]
    return {
        "stable": stable,
        "reasons": reasons,
        "latency_avg_ms": sum(latency_values) / len(latency_values) if latency_values else 0.0,
        "drift_activity": max((abs(numeric(row, "resampler_ratio") - 1.0) for row in finals if row), default=0.0),
    }


def evaluate_candidate(base_logs, candidate, run_results):
    stable_runs = [result for result in run_results if result["evaluation"]["stable"]]
    all_reasons = sorted({reason for result in run_results for reason in result["evaluation"]["reasons"]})
    latency = [result["evaluation"]["latency_avg_ms"] for result in run_results if result["evaluation"]["latency_avg_ms"] > 0]
    return {
        "candidate": candidate.id,
        "stable": len(stable_runs) == len(run_results),
        "stable_runs": len(stable_runs),
        "run_count": len(run_results),
        "reasons": all_reasons,
        "latency_avg_ms": sum(latency) / len(latency) if latency else 0.0,
    }


def write_summary(base_logs, rows, recommendation):
    analysis_path = base_logs / "analysis.json"
    write_json(analysis_path, {
        "recommendation": recommendation,
        "candidates": rows,
    })
    text_path = base_logs / "recommendation.txt"
    if recommendation is None:
        text_path.write_text("No mostly stable profile found.\n", encoding="utf-8")
    else:
        text_path.write_text(
            "Recommended profile:\n"
            f"  {recommendation['candidate']}\n"
            f"  latency_avg_ms={recommendation['latency_avg_ms']:.2f}\n"
            f"  stable_runs={recommendation['stable_runs']}/{recommendation['run_count']}\n",
            encoding="utf-8")
    try:
        combined, written, file_count = collect_matrix_csv(base_logs, side="all")
        print_flush(f"[server] combined {written} row(s) from {file_count} CSV file(s): {combined}")
        analysis_csv, analysis_rows = analyze_matrix_csv(combined, print_top=True)
        print_flush(f"[server] wrote CSV analysis for {len(analysis_rows)} profile(s): {analysis_csv}")
    except SystemExit as error:
        print_flush(f"[server] CSV analysis skipped: {error}")
    print_flush(f"[server] wrote adaptive analysis: {analysis_path}")


def run_candidate(jam2, candidate, stream_ms, base_logs, public_dir, audio_device, sample_rate, no_stun, start_run_index=1):
    results = []
    for run_index in range(start_run_index, start_run_index + RUNS_PER_CANDIDATE):
        server_rc = run_listen(jam2, candidate, run_index, stream_ms, base_logs, public_dir, audio_device, sample_rate, no_stun)
        client_uploaded = wait_for_client_upload(base_logs, candidate.id, run_index)
        server = side_result(base_logs, candidate.id, "server", run_index)
        client = side_result(base_logs, candidate.id, "client", run_index)
        evaluation = evaluate_run(server, client, server_rc, client_uploaded)
        print_flush(
            f"[server] evaluated {candidate.id} run {run_index}: "
            f"stable={evaluation['stable']} reasons={','.join(evaluation['reasons']) or 'none'}")
        results.append({
            "run_index": run_index,
            "server_rc": server_rc,
            "client_uploaded": client_uploaded,
            "evaluation": evaluation,
        })
    return results


def should_try_drift_variant(candidate_summary):
    return any("playback_depth_low" in reason or "jitter_spike" in reason for reason in candidate_summary["reasons"])


def main():
    args_ns = parse_args()
    jam2 = Path(args_ns.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    base_logs_path = Path(args_ns.logs)
    if args_ns.clean and base_logs_path.exists():
        shutil.rmtree(base_logs_path)
    base_logs = ensure_dir(base_logs_path)
    public_dir = ensure_dir(base_logs / "public")
    server = start_http_server(public_dir, base_logs, args_ns.host, args_ns.port)
    summaries = []
    recommendation = None
    print_flush(f"[server] publishing {public_dir} on http://{args_ns.host}:{args_ns.port}/")
    try:
        for base_candidate in candidate_ladder():
            candidates = [base_candidate]
            for candidate in candidates:
                results = run_candidate(
                    jam2,
                    candidate,
                    SHORT_STREAM_MS,
                    base_logs,
                    public_dir,
                    args_ns.server_audio_device,
                    args_ns.sample_rate,
                    args_ns.no_stun)
                summary = evaluate_candidate(base_logs, candidate, results)
                summaries.append(summary)
                print_flush(
                    f"[server] candidate {candidate.id}: stable={summary['stable']} "
                    f"runs={summary['stable_runs']}/{summary['run_count']} "
                    f"reasons={','.join(summary['reasons']) or 'none'}")
                if summary["stable"]:
                    confirm_results = run_candidate(
                        jam2,
                        candidate,
                        CONFIRM_STREAM_MS,
                        base_logs,
                        public_dir,
                        args_ns.server_audio_device,
                        args_ns.sample_rate,
                        args_ns.no_stun,
                        101)
                    confirm_summary = evaluate_candidate(base_logs, candidate, confirm_results)
                    confirm_summary["confirmation"] = True
                    summaries.append(confirm_summary)
                    if confirm_summary["stable"]:
                        recommendation = confirm_summary
                        write_summary(base_logs, summaries, recommendation)
                        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
                        return 0
                    print_flush(f"[server] confirmation failed for {candidate.id}; continuing backoff")
                    break
                if candidate == base_candidate and should_try_drift_variant(summary):
                    for variant in list(drift_variants(base_candidate))[1:]:
                        variant_results = run_candidate(
                            jam2,
                            variant,
                            SHORT_STREAM_MS,
                            base_logs,
                            public_dir,
                            args_ns.server_audio_device,
                            args_ns.sample_rate,
                            args_ns.no_stun)
                        variant_summary = evaluate_candidate(base_logs, variant, variant_results)
                        summaries.append(variant_summary)
                        if variant_summary["stable"]:
                            confirm_results = run_candidate(
                                jam2,
                                variant,
                                CONFIRM_STREAM_MS,
                                base_logs,
                                public_dir,
                                args_ns.server_audio_device,
                                args_ns.sample_rate,
                                args_ns.no_stun,
                                101)
                            confirm_summary = evaluate_candidate(base_logs, variant, confirm_results)
                            confirm_summary["confirmation"] = True
                            summaries.append(confirm_summary)
                            if confirm_summary["stable"]:
                                recommendation = confirm_summary
                                write_summary(base_logs, summaries, recommendation)
                                write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
                                return 0
            write_summary(base_logs, summaries, recommendation)
        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
        return 1
    finally:
        server.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
