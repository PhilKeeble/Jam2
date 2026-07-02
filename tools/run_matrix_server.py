#!/usr/bin/env python3

import argparse
import csv
import http.server
import json
import math
import shlex
import shutil
import subprocess
import sys
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


PREFILL_VALUES = [768, 1024, 1536, 2048]
FRAME_VALUES = [128, 256]
AUDIO_BUFFER_VALUES = [32, 64, 128, 256]
NORMAL_AUDIO_BUFFER_VALUES = [32, 64]
LOW_LATENCY_BASELINE_PROFILES = [
    (32, 128, 768),
    (64, 128, 768),
    (32, 128, 1024),
    (32, 128, 1536),
]
PROBE_RUNS = 1
EDGE_RUNS = 3
CONFIRM_RUNS = 3
INFRA_RETRIES = 1
CONFIRM_BURST_RETRIES = 1
SHORT_STREAM_MS = 30000
CONFIRM_STREAM_MS = 60000
STATS_INTERVAL_MS = 5000
STATS_WARMUP_MS = 3000
CLIENT_UPLOAD_TIMEOUT_S = 30
STABLE_HARD_PLAYBACK_DEPTH_MIN_MS = 2.5
STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS = 5.0
PROBE_PLAYBACK_DEPTH_MIN_MS = STABLE_HARD_PLAYBACK_DEPTH_MIN_MS
STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND = 3.0
STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND = 0.10
STABLE_MAX_PLAYBACK_UNDERRUNS_PER_RUN = 4096
STABLE_MIN_GOOD_RUN_RATIO = 2.0 / 3.0
STABLE_MAX_PACKET_LOSS_PERCENT = 0.05
STABLE_MAX_PACKET_LOSS_PER_SECOND = 0.10
STABLE_MAX_PACKET_LOSS_PER_RUN = 16
STABLE_MAX_DROPPED_FRAMES_PER_SECOND = 8.0
STABLE_MAX_DROPPED_FRAMES_PER_RUN = 2048
PLAYABLE_MIN_RUNS = EDGE_RUNS
PLAYABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND = 10.0
PLAYABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND = 2.0
PLAYABLE_MAX_PLAYBACK_UNDERRUNS_PER_RUN = 32768
PLAYABLE_MIN_PLAYBACK_DEPTH_MS = STABLE_HARD_PLAYBACK_DEPTH_MIN_MS
PLAYABLE_MAX_JITTER_MS = 120.0
PLAYABLE_MAX_PACKET_LOSS_PERCENT = 0.20
PLAYABLE_MAX_PACKET_LOSS_PER_SECOND = 0.35
PLAYABLE_MAX_DROPPED_FRAMES_PER_SECOND = 4.0
PLAYABLE_MAX_DROPPED_FRAME_PERCENT = 0.02
PRESSURED_MAX_PLAYBACK_UNDERRUNS_PER_SECOND = 80.0
PRESSURED_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND = 80.0
PRESSURED_MAX_PACKET_LOSS_PERCENT = 0.25
PRESSURED_MAX_PACKET_LOSS_PER_SECOND = 0.70
PRESSURED_MAX_DROPPED_FRAMES_PER_SECOND = 0.25
PRESSURED_MAX_DROPPED_FRAME_PERCENT = 0.10
PRESSURED_MAX_JITTER_MS = 80.0
EXPERIMENTAL_MAX_PACKET_LOSS_PERCENT = 0.40
EXPERIMENTAL_MAX_PACKET_LOSS_PER_SECOND = 1.50
EXPERIMENTAL_MAX_DROPPED_FRAMES_PER_SECOND = 500.0
EXPERIMENTAL_MAX_DROPPED_FRAME_PERCENT = 0.75
EXPERIMENTAL_MAX_JITTER_MS = 180.0
DRIFT_OFF_FINAL_PPM_LIMIT = 15.0
DRIFT_OFF_PERIODIC_PPM_LIMIT = 50.0
DRIFT_OFF_DEPTH_DELTA_MS_LIMIT = 2.0
DRIFT_VARIANT_LATENCY_MARGIN_MS = 1.0
DRIFT_DEADBAND_STEP_PPM = 25
DRIFT_DEADBAND_CUSHION_PPM = 10
DRIFT_DEADBAND_MAX_PPM = 200
AGGRESSIVE_MIN_RUNS = 3
AGGRESSIVE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND = 8.0
AGGRESSIVE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND = 0.25
AGGRESSIVE_MAX_PLAYBACK_UNDERRUNS_PER_RUN = 16384
AGGRESSIVE_MIN_PLAYBACK_DEPTH_MS = 2.5
AGGRESSIVE_MAX_JITTER_MS = 60.0
AGGRESSIVE_MAX_PACKET_LOSS_PERCENT = 0.10
AGGRESSIVE_MAX_PACKET_LOSS_PER_SECOND = 0.25
AGGRESSIVE_MAX_DROPPED_FRAMES_PER_SECOND = 12.0
BENCHMARK_RUNS = 3
BENCHMARK_STREAM_MS = 30000
BENCHMARK_AUDIO_BUFFER_VALUES = [32, 64, 128, 256]
BENCHMARK_FRAME_VALUES = [64, 128, 256]
BENCHMARK_PREFILL_VALUES = [768, 1024, 1536, 2048, 4096]
BENCHMARK_MAX_PROFILES = 16
BENCHMARK_BURST_RETRIES = 3


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
        playback_max_frames = max(self.playback_max_frames, self.playback_prefill_frames * 2)
        return [
            "--audio-buffer-size", str(self.audio_buffer_size),
            "--frame-size", str(self.frame_size),
            "--playback-prefill-frames", str(self.playback_prefill_frames),
            "--playback-ring-frames", str(self.playback_ring_frames),
            "--playback-max-frames", str(playback_max_frames),
            "--drift-correction", self.drift_correction,
            "--drift-deadband-ppm", str(self.drift_deadband_ppm),
            "--stats-warmup-ms", str(STATS_WARMUP_MS),
            "--stats-interval-ms", str(STATS_INTERVAL_MS),
            "--stream-ms", str(stream_ms),
            "--stream-linger-ms", "500",
        ]

    def runtime_args(self):
        playback_max_frames = max(self.playback_max_frames, self.playback_prefill_frames * 2)
        return [
            "--audio-buffer-size", str(self.audio_buffer_size),
            "--frame-size", str(self.frame_size),
            "--playback-prefill-frames", str(self.playback_prefill_frames),
            "--playback-ring-frames", str(self.playback_ring_frames),
            "--playback-max-frames", str(playback_max_frames),
            "--drift-correction", self.drift_correction,
            "--drift-deadband-ppm", str(self.drift_deadband_ppm),
        ]

    def profile(self):
        return {
            "audio_buffer_size": self.audio_buffer_size,
            "frame_size": self.frame_size,
            "playback_prefill_frames": self.playback_prefill_frames,
            "playback_ring_frames": self.playback_ring_frames,
            "playback_max_frames": max(self.playback_max_frames, self.playback_prefill_frames * 2),
            "drift_correction": self.drift_correction,
            "drift_deadband_ppm": self.drift_deadband_ppm,
        }


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
            "network_type": payload.get("network_type", "unknown"),
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
    parser.add_argument(
        "--network-profile",
        choices=("auto", "wired", "wifi", "unknown"),
        default="auto",
        help="network type for tuner tolerances; auto uses best-effort local detection")
    parser.add_argument("--drift-probes", action="store_true", help="test one measured drift-deadband variant after a physical profile is stable")
    parser.add_argument("--benchmark", action="store_true", help="run a targeted connection benchmark instead of adaptive early-stop tuning")
    return parser.parse_args()


def start_http_server(public_dir, logs_dir, host, port):
    MatrixHttpHandler.public_dir = public_dir
    MatrixHttpHandler.logs_dir = logs_dir
    server = http.server.ThreadingHTTPServer((host, port), MatrixHttpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def candidate_ladder():
    seen = set()
    for profile in LOW_LATENCY_BASELINE_PROFILES:
        candidate = Candidate(*profile)
        if candidate.id in seen:
            continue
        seen.add(candidate.id)
        yield candidate
    for audio_buffer in NORMAL_AUDIO_BUFFER_VALUES:
        for prefill in PREFILL_VALUES:
            for frame_size in FRAME_VALUES:
                if frame_size < audio_buffer:
                    continue
                candidate = Candidate(audio_buffer, frame_size, prefill)
                if candidate.id in seen:
                    continue
                seen.add(candidate.id)
                yield candidate


def benchmark_candidate(audio_buffer, frame_size, prefill):
    playback_max = max(2048, prefill * 2)
    playback_ring = max(4096, playback_max * 2)
    return Candidate(audio_buffer, frame_size, prefill, playback_ring, playback_max)


def benchmark_baseline_candidates():
    return [
        benchmark_candidate(32, 64, 768),
        benchmark_candidate(64, 64, 768),
        benchmark_candidate(32, 128, 1024),
        benchmark_candidate(64, 128, 1024),
        benchmark_candidate(128, 128, 1024),
        benchmark_candidate(128, 128, 1536),
        benchmark_candidate(128, 256, 2048),
        benchmark_candidate(128, 256, 4096),
    ]


def profile_key(candidate):
    return (candidate.audio_buffer_size, candidate.frame_size, candidate.playback_prefill_frames)


def is_low_latency_baseline(candidate):
    return profile_key(candidate) in LOW_LATENCY_BASELINE_PROFILES


def low_latency_baseline_complete(candidate_stages):
    for audio_buffer, frame_size, prefill in LOW_LATENCY_BASELINE_PROFILES:
        candidate = Candidate(audio_buffer, frame_size, prefill)
        if candidate_stage_rank(candidate_stages.get(candidate.id, "")) < candidate_stage_rank("edge"):
            return False
    return True


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
            blocks = ports.split("\n\n")
            for block in blocks:
                if f"Device: {iface}" in block:
                    lower = block.lower()
                    if "wi-fi" in lower or "airport" in lower:
                        return "wifi"
                    return "wired"
    except Exception:
        pass
    return "unknown"


def effective_network_profile(requested, detected_server, detected_client):
    if detected_server == "wifi" or detected_client == "wifi":
        return "wifi"
    if requested in ("wired", "wifi"):
        return requested
    if detected_server == "wired" and detected_client == "wired":
        return "wired"
    return "unknown"


def network_profile_note(requested, effective, detected_server, detected_client):
    if requested != effective and effective == "wifi":
        return (
            " wifi_overrides_requested_profile"
            f"({requested}) because server={detected_server} client={detected_client}")
    return ""


def same_physical_candidate(candidate, drift_correction, drift_deadband_ppm):
    return Candidate(
        candidate.audio_buffer_size,
        candidate.frame_size,
        candidate.playback_prefill_frames,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        drift_correction,
        drift_deadband_ppm)


def candidate_from_profile(profile):
    return Candidate(
        int(profile["audio_buffer_size"]),
        int(profile["frame_size"]),
        int(profile["playback_prefill_frames"]),
        int(profile["playback_ring_frames"]),
        int(profile["playback_max_frames"]),
        profile["drift_correction"],
        int(profile["drift_deadband_ppm"]))


def next_value(values, current):
    for value in values:
        if value > current:
            return value
    return None


def previous_value(values, current):
    previous = None
    for value in values:
        if value >= current:
            return previous
        previous = value
    return previous


def benchmark_next_value(values, current):
    return next_value(values, current)


def benchmark_previous_value(values, current):
    return previous_value(values, current)


def benchmark_valid_candidate(candidate):
    return (
        candidate.audio_buffer_size in BENCHMARK_AUDIO_BUFFER_VALUES
        and candidate.frame_size in BENCHMARK_FRAME_VALUES
        and candidate.playback_prefill_frames in BENCHMARK_PREFILL_VALUES
        and candidate.frame_size >= candidate.audio_buffer_size
    )


def benchmark_with_audio_buffer(candidate, audio_buffer):
    if audio_buffer is None:
        return None
    return benchmark_candidate(audio_buffer, candidate.frame_size, candidate.playback_prefill_frames)


def benchmark_with_frame_size(candidate, frame_size):
    if frame_size is None:
        return None
    return benchmark_candidate(candidate.audio_buffer_size, frame_size, candidate.playback_prefill_frames)


def benchmark_with_prefill(candidate, prefill):
    if prefill is None:
        return None
    return benchmark_candidate(candidate.audio_buffer_size, candidate.frame_size, prefill)


def valid_candidate(candidate):
    return (
        candidate.audio_buffer_size in AUDIO_BUFFER_VALUES
        and candidate.frame_size in FRAME_VALUES
        and candidate.playback_prefill_frames in PREFILL_VALUES
        and candidate.frame_size >= candidate.audio_buffer_size
    )


def with_audio_buffer(candidate, audio_buffer):
    return Candidate(
        audio_buffer,
        candidate.frame_size,
        candidate.playback_prefill_frames,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        candidate.drift_correction,
        candidate.drift_deadband_ppm)


def with_frame_size(candidate, frame_size):
    return Candidate(
        candidate.audio_buffer_size,
        frame_size,
        candidate.playback_prefill_frames,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        candidate.drift_correction,
        candidate.drift_deadband_ppm)


def with_prefill(candidate, prefill):
    return Candidate(
        candidate.audio_buffer_size,
        candidate.frame_size,
        prefill,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        candidate.drift_correction,
        candidate.drift_deadband_ppm)


def numeric(row, field):
    try:
        return float(row.get(field, "") or 0)
    except ValueError:
        return 0.0


def packet_loss_count(row_or_metrics):
    return max(
        float(row_or_metrics.get("sequence_lost", 0.0) or 0.0),
        float(row_or_metrics.get("reordered_lost", 0.0) or 0.0))


def playback_underrun_rates(row):
    frames = numeric(row, "playback_ring_underruns")
    events = numeric(row, "playback_ring_underrun_events")
    elapsed_seconds = numeric(row, "elapsed_ms") / 1000.0
    frames_per_second = frames / elapsed_seconds if elapsed_seconds > 0.0 else 0.0
    events_per_second = events / elapsed_seconds if elapsed_seconds > 0.0 else 0.0
    if frames > 0.0 and events <= 0.0:
        audio_buffer_frames = max(1.0, numeric(row, "requested_audio_buffer_frames"))
        events_per_second = frames_per_second / audio_buffer_frames
    return frames, frames_per_second, events_per_second


def stable_underrun_failure(row):
    if has_field(row, "playback_depth_observed_frames"):
        return (
            numeric(row, "playback_ring_underrun_time_percent") > 0.05
            or numeric(row, "playback_ring_underrun_burst_max_ms") > 10.0
        )
    frames, frames_per_second, events_per_second = playback_underrun_rates(row)
    return (
        frames > STABLE_MAX_PLAYBACK_UNDERRUNS_PER_RUN
        or frames_per_second > STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND
        or events_per_second > STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND
    )


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
    target = run_dir(base_logs, candidate_id, "client", run_index) / "upload.json"
    deadline = time.monotonic() + CLIENT_UPLOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        if target.exists():
            return True
        time.sleep(0.25)
    return False


def client_network_type_for_run(base_logs, candidate_id, run_index):
    path = run_dir(base_logs, candidate_id, "client", run_index) / "upload.json"
    if not path.exists():
        return "unknown"
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("network_type", "unknown")
    except (OSError, json.JSONDecodeError):
        return "unknown"


def run_listen(jam2, candidate, run_index, stream_ms, base_logs, public_dir, audio_device, sample_rate, no_stun, wait_for_initial_client, network_profile, server_network_type):
    output_dir = run_dir(base_logs, candidate.id, "server", run_index)
    csv_dir = ensure_dir(output_dir / "csv_raw")
    stdout_path = output_dir / "stdout.txt"
    stderr_path = output_dir / "stderr.txt"
    candidate_args = candidate.args(stream_ms)
    write_json(output_dir / "metadata.json", {
        "network_type": server_network_type,
        "network_profile": network_profile,
    })

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
    if not wait_for_initial_client:
        args.extend(["--wait-ms", "30000"])

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


def packet_loss_ok(row, profile):
    lost = packet_loss_count(row)
    elapsed_seconds = numeric(row, "elapsed_ms") / 1000.0
    recv_packets = max(1.0, numeric(row, "recv_packets") + numeric(row, "sequence_lost"))
    loss_percent = lost * 100.0 / recv_packets
    loss_per_second = lost / elapsed_seconds if elapsed_seconds > 0.0 else lost
    if lost <= 0.0:
        return True
    if profile == "wifi":
        return (
            loss_percent <= STABLE_MAX_PACKET_LOSS_PERCENT
            and loss_per_second <= STABLE_MAX_PACKET_LOSS_PER_SECOND
            and lost <= STABLE_MAX_PACKET_LOSS_PER_RUN
        )
    return False


def dropped_frames_ok(row, profile):
    frames = numeric(row, "playback_dropped_frames")
    elapsed_seconds = numeric(row, "elapsed_ms") / 1000.0
    frames_per_second = frames / elapsed_seconds if elapsed_seconds > 0.0 else frames
    if frames <= 0.0:
        return True
    if profile == "wifi":
        return (
            frames_per_second <= STABLE_MAX_DROPPED_FRAMES_PER_SECOND
            and frames <= STABLE_MAX_DROPPED_FRAMES_PER_RUN
        )
    return False


def evaluate_run(server, client, server_rc, client_uploaded, network_profile="unknown"):
    reasons = []
    warnings = []
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
            if packet_loss_ok(row, network_profile):
                loss = numeric(row, "sequence_lost") + numeric(row, "reordered_lost")
                warnings.append(f"{label}_packet_loss_tiny({loss:.0f}_packets)")
            else:
                reasons.append(f"{label}_packet_loss")
        if numeric(row, "playback_ring_underruns") > 0:
            if stable_underrun_failure(row):
                reasons.append(f"{label}_playback_underrun")
            else:
                frames, frames_per_second, events_per_second = playback_underrun_rates(row)
                warnings.append(
                    f"{label}_playback_underrun_tiny"
                    f"({frames:.0f}_frames,{frames_per_second:.3f}_fps,{events_per_second:.3f}_eps)")
        if numeric(row, "playback_ring_overruns") > 0:
            reasons.append(f"{label}_playback_overrun")
        if numeric(row, "playback_dropped_frames") > 0:
            if dropped_frames_ok(row, network_profile):
                warnings.append(f"{label}_playback_dropped_tiny({numeric(row, 'playback_dropped_frames'):.0f}_frames)")
            else:
                reasons.append(f"{label}_playback_dropped")
        playback_depth_min_ms = numeric(row, "playback_depth_min_ms")
        if playback_depth_min_ms < STABLE_HARD_PLAYBACK_DEPTH_MIN_MS:
            reasons.append(f"{label}_playback_depth_low")
        elif playback_depth_min_ms < STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS:
            warnings.append(f"{label}_playback_depth_near_empty({playback_depth_min_ms:.2f}ms)")
        if numeric(row, "jitter_max_ms") > 12.0:
            warnings.append(f"{label}_jitter_spike")
    if repeating_capture_underruns(server["rows"]):
        reasons.append("server_repeating_capture_underruns")
    if repeating_capture_underruns(client["rows"]):
        reasons.append("client_repeating_capture_underruns")

    stable = not reasons
    latency_values = [numeric(row, "estimated_one_way_ms") for row in finals if row]
    return {
        "stable": stable,
        "reasons": reasons,
        "warnings": warnings,
        "latency_avg_ms": sum(latency_values) / len(latency_values) if latency_values else 0.0,
        "drift_activity": max((abs(numeric(row, "resampler_ratio") - 1.0) for row in finals if row), default=0.0),
    }


def has_probe_hard_failure(evaluation):
    hard_prefixes = (
        "server_rc=",
        "client_upload_missing",
        "server_final_missing",
        "client_final_missing",
        "server_packet_loss",
        "client_packet_loss",
        "server_playback_underrun",
        "client_playback_underrun",
        "server_playback_overrun",
        "client_playback_overrun",
        "server_playback_dropped",
        "client_playback_dropped",
        "server_repeating_capture_underruns",
        "client_repeating_capture_underruns",
    )
    return any(reason.startswith(hard_prefixes) for reason in evaluation["reasons"])


def is_infrastructure_failure(result):
    reasons = result["evaluation"]["reasons"]
    return any(
        reason.startswith("server_rc=") or reason in (
            "client_upload_missing",
            "server_final_missing",
            "client_final_missing",
        )
        for reason in reasons
    )


def has_probe_depth_failure(server, client):
    depths = []
    for side in (server, client):
        row = side["final"]
        if row:
            depths.append(numeric(row, "playback_depth_min_ms"))
    return not depths or min(depths) < PROBE_PLAYBACK_DEPTH_MIN_MS


def is_practical_probe(result):
    return (
        result.get("network_profile") == "wifi"
        and not is_infrastructure_failure(result)
        and practical_accepts(result.get("metrics", {}), "wifi")
    )


def is_promising_probe(result):
    return (
        (
            not has_probe_hard_failure(result["evaluation"])
            or is_practical_probe(result)
        )
        and not result.get("probe_depth_failure", False)
    )


def is_depth_only_failure(evaluation):
    reasons = evaluation["reasons"]
    return bool(reasons) and all(reason in (
        "server_playback_depth_low",
        "client_playback_depth_low",
    ) for reason in reasons)


def next_prefill_value(current_prefill, target_prefill):
    for value in PREFILL_VALUES:
        if value > current_prefill and value >= target_prefill:
            return value
    return None


def prefill_jump_candidate(candidate, probe_result, sample_rate):
    if not is_depth_only_failure(probe_result["evaluation"]):
        return None
    observed_depth = probe_result.get("metrics", {}).get("min_playback_depth_ms", 0.0)
    if observed_depth <= 0.0 or observed_depth >= STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS:
        return None
    shortfall_ms = STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS - observed_depth
    extra_frames = int(math.ceil(shortfall_ms * sample_rate / 1000.0))
    target_prefill = candidate.playback_prefill_frames + extra_frames
    prefill = next_prefill_value(candidate.playback_prefill_frames, target_prefill)
    if prefill is None:
        return None
    return Candidate(
        candidate.audio_buffer_size,
        candidate.frame_size,
        prefill,
        candidate.playback_ring_frames,
        candidate.playback_max_frames,
        candidate.drift_correction,
        candidate.drift_deadband_ppm)


def queue_prefill_jump(
    candidate,
    run_results,
    sample_rate,
    pending_candidates,
    queued_candidate_ids,
    candidate_stages,
    phase):
    for result in run_results:
        jump_candidate = prefill_jump_candidate(candidate, result, sample_rate)
        if jump_candidate is None:
            continue
        if not can_queue_candidate(jump_candidate, candidate_stages, queued_candidate_ids, allow_probe_retry=True):
            return False
        pending_candidates.insert(0, jump_candidate)
        queued_candidate_ids.add(jump_candidate.id)
        metrics = result["metrics"]
        print_flush(
            f"[server] queued data-driven prefill jump after {phase} {jump_candidate.id}: "
            f"depth_min_ms={metrics['min_playback_depth_ms']:.2f} "
            f"target_depth_ms={STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS:.2f}")
        return True
    return False


def queue_smart_candidates(
    candidate,
    summary,
    previous_summary,
    pending_candidates,
    queued_candidate_ids,
    candidate_stages,
    phase):
    queued = False
    for next_candidate in reversed(smart_next_candidates(candidate, summary, previous_summary)):
        if not can_queue_candidate(next_candidate, candidate_stages, queued_candidate_ids, allow_probe_retry=True):
            continue
        pending_candidates.insert(0, next_candidate)
        queued_candidate_ids.add(next_candidate.id)
        print_flush(
            f"[server] queued data-driven next candidate after {phase}: "
            f"{next_candidate.id} from {candidate.id}")
        queued = True
    return queued


def candidate_stage_rank(stage):
    return {
        "": 0,
        "queued": 0,
        "probe": 1,
        "edge": 2,
        "confirm": 3,
    }.get(stage or "", 0)


def can_queue_candidate(candidate, candidate_stages, queued_candidate_ids, allow_probe_retry=False):
    stage = candidate_stages.get(candidate.id, "")
    if candidate.id in queued_candidate_ids:
        return False
    if not stage:
        return True
    return allow_probe_retry and stage == "probe"


def ratio_delta_ppm(row):
    return abs(numeric(row, "resampler_ratio") - 1.0) * 1_000_000.0


def sample_rate_for_row(row):
    return (
        numeric(row, "active_sample_rate")
        or numeric(row, "requested_sample_rate")
        or numeric(row, "sample_rate")
        or 44100.0)


def has_field(row, field):
    return row.get(field, "") not in ("", None)


def run_metrics(server, client):
    final_rows = [side["final"] for side in (server, client) if side["final"]]
    periodic_rows_all = server["periodic"] + client["periodic"]
    elapsed_seconds = sum((numeric(row, "elapsed_ms") for row in final_rows), 0.0) / 1000.0
    audio_frame_seconds = sum(
        (numeric(row, "elapsed_ms") / 1000.0) * max(1.0, sample_rate_for_row(row))
        for row in final_rows)
    playback_underruns = sum((numeric(row, "playback_ring_underruns") for row in final_rows), 0.0)
    playback_underrun_events = sum((numeric(row, "playback_ring_underrun_events") for row in final_rows), 0.0)
    playback_depth_observed_frames = sum((numeric(row, "playback_depth_observed_frames") for row in final_rows), 0.0)
    playback_underrun_time_percent = (
        playback_underruns * 100.0 / playback_depth_observed_frames
        if playback_depth_observed_frames > 0.0 else 0.0)
    playback_underrun_time_ms = sum((numeric(row, "playback_ring_underrun_time_ms") for row in final_rows), 0.0)
    if playback_underrun_time_ms <= 0.0:
        playback_underrun_time_ms = sum(
            numeric(row, "playback_ring_underruns") * 1000.0 / sample_rate_for_row(row)
            for row in final_rows)
    underrun_events_for_rate = playback_underrun_events
    if playback_underruns > 0.0 and playback_underrun_events <= 0.0:
        underrun_events_for_rate = sum(
            playback_underrun_rates(row)[2] * (numeric(row, "elapsed_ms") / 1000.0)
            for row in final_rows)
    depth_deltas = []
    for side in (server, client):
        depths = [numeric(row, "playback_depth_avg_ms") for row in side["periodic"]]
        if len(depths) >= 2:
            depth_deltas.append(depths[-1] - depths[0])
    return {
        "sequence_lost": sum((numeric(row, "sequence_lost") for row in final_rows), 0.0),
        "reordered_lost": sum((numeric(row, "reordered_lost") for row in final_rows), 0.0),
        "packet_loss": sum((packet_loss_count(row) for row in final_rows), 0.0),
        "recv_packets": sum((numeric(row, "recv_packets") for row in final_rows), 0.0),
        "elapsed_seconds": elapsed_seconds,
        "playback_ring_underruns": playback_underruns,
        "playback_ring_underrun_events": playback_underrun_events,
        "playback_ring_underrun_time_ms": playback_underrun_time_ms,
        "playback_ring_underrun_time_percent": playback_underrun_time_percent,
        "playback_ring_underrun_event_max_ms": max(
            (numeric(row, "playback_ring_underrun_event_max_ms") for row in final_rows),
            default=0.0),
        "playback_ring_underrun_burst_max_ms": max(
            (numeric(row, "playback_ring_underrun_burst_max_ms") for row in final_rows),
            default=0.0),
        "playback_depth_observed_frames": playback_depth_observed_frames,
        "has_playback_severity_stats": any(has_field(row, "playback_depth_observed_frames") for row in final_rows),
        "playback_ring_underruns_per_second": playback_underruns / elapsed_seconds if elapsed_seconds > 0.0 else 0.0,
        "playback_ring_underrun_events_per_second": (
            underrun_events_for_rate / elapsed_seconds if elapsed_seconds > 0.0 else 0.0),
        "max_playback_ring_underruns": max((numeric(row, "playback_ring_underruns") for row in final_rows), default=0.0),
        "playback_ring_overruns": sum((numeric(row, "playback_ring_overruns") for row in final_rows), 0.0),
        "playback_dropped_frames": sum((numeric(row, "playback_dropped_frames") for row in final_rows), 0.0),
        "playback_drop_events": sum((numeric(row, "playback_drop_events") for row in final_rows), 0.0),
        "playback_drop_event_max_ms": max((numeric(row, "playback_drop_event_max_ms") for row in final_rows), default=0.0),
        "playback_dropped_time_ms": sum((numeric(row, "playback_dropped_time_ms") for row in final_rows), 0.0),
        "audio_frame_seconds": audio_frame_seconds,
        "playback_dropped_frame_percent": (
            sum((numeric(row, "playback_dropped_frames") for row in final_rows), 0.0) * 100.0 / max(1.0, audio_frame_seconds)),
        "jitter_max_ms": max((numeric(row, "jitter_max_ms") for row in final_rows), default=0.0),
        "max_abs_final_drift_ppm": max((abs(numeric(row, "drift_ppm")) for row in final_rows), default=0.0),
        "max_abs_periodic_drift_ppm": max((abs(numeric(row, "drift_ppm")) for row in periodic_rows_all), default=0.0),
        "max_abs_final_ratio_delta_ppm": max((ratio_delta_ppm(row) for row in final_rows), default=0.0),
        "max_abs_ratio_delta_ppm": max(
            (ratio_delta_ppm(row) for row in final_rows + periodic_rows_all),
            default=0.0),
        "min_playback_depth_ms": min((numeric(row, "playback_depth_min_ms") for row in final_rows), default=0.0),
        "max_abs_depth_delta_ms": max((abs(value) for value in depth_deltas), default=0.0),
    }


def aggregate_metrics(run_results):
    metrics = [result.get("metrics", {}) for result in run_results]
    elapsed_seconds = sum((metric.get("elapsed_seconds", 0.0) for metric in metrics), 0.0)
    playback_underruns = sum((metric.get("playback_ring_underruns", 0.0) for metric in metrics), 0.0)
    playback_underrun_events = sum((metric.get("playback_ring_underrun_events", 0.0) for metric in metrics), 0.0)
    playback_depth_observed_frames = sum((metric.get("playback_depth_observed_frames", 0.0) for metric in metrics), 0.0)
    playback_underrun_time_ms = sum((metric.get("playback_ring_underrun_time_ms", 0.0) for metric in metrics), 0.0)
    playback_underrun_time_percent = (
        playback_underruns * 100.0 / playback_depth_observed_frames
        if playback_depth_observed_frames > 0.0 else 0.0)
    weighted_underrun_event_rate = sum(
        metric.get("playback_ring_underrun_events_per_second", 0.0) * metric.get("elapsed_seconds", 0.0)
        for metric in metrics)
    sequence_lost = sum((metric.get("sequence_lost", 0.0) for metric in metrics), 0.0)
    reordered_lost = sum((metric.get("reordered_lost", 0.0) for metric in metrics), 0.0)
    packet_loss = sum((metric.get("packet_loss", 0.0) for metric in metrics), 0.0)
    recv_packets = sum((metric.get("recv_packets", 0.0) for metric in metrics), 0.0)
    dropped_frames = sum((metric.get("playback_dropped_frames", 0.0) for metric in metrics), 0.0)
    dropped_time_ms = sum((metric.get("playback_dropped_time_ms", 0.0) for metric in metrics), 0.0)
    audio_frame_seconds = sum((metric.get("audio_frame_seconds", 0.0) for metric in metrics), 0.0)
    return {
        "sequence_lost": sequence_lost,
        "reordered_lost": reordered_lost,
        "packet_loss": packet_loss,
        "packet_loss_percent": packet_loss * 100.0 / max(1.0, recv_packets + sequence_lost),
        "packet_loss_per_second": packet_loss / elapsed_seconds if elapsed_seconds > 0.0 else packet_loss,
        "elapsed_seconds": elapsed_seconds,
        "playback_ring_underruns": playback_underruns,
        "playback_ring_underrun_events": playback_underrun_events,
        "playback_ring_underrun_time_ms": playback_underrun_time_ms,
        "playback_ring_underrun_time_percent": playback_underrun_time_percent,
        "playback_ring_underrun_event_max_ms": max(
            (metric.get("playback_ring_underrun_event_max_ms", 0.0) for metric in metrics),
            default=0.0),
        "playback_ring_underrun_burst_max_ms": max(
            (metric.get("playback_ring_underrun_burst_max_ms", 0.0) for metric in metrics),
            default=0.0),
        "playback_depth_observed_frames": playback_depth_observed_frames,
        "has_playback_severity_stats": any(metric.get("has_playback_severity_stats", False) for metric in metrics),
        "playback_ring_underruns_per_second": playback_underruns / elapsed_seconds if elapsed_seconds > 0.0 else 0.0,
        "playback_ring_underrun_events_per_second": (
            weighted_underrun_event_rate / elapsed_seconds if elapsed_seconds > 0.0 else 0.0),
        "max_playback_ring_underruns": max((metric.get("max_playback_ring_underruns", 0.0) for metric in metrics), default=0.0),
        "playback_ring_overruns": sum((metric.get("playback_ring_overruns", 0.0) for metric in metrics), 0.0),
        "playback_dropped_frames": dropped_frames,
        "playback_drop_events": sum((metric.get("playback_drop_events", 0.0) for metric in metrics), 0.0),
        "playback_drop_event_max_ms": max((metric.get("playback_drop_event_max_ms", 0.0) for metric in metrics), default=0.0),
        "playback_dropped_time_ms": dropped_time_ms,
        "playback_dropped_frames_per_second": dropped_frames / elapsed_seconds if elapsed_seconds > 0.0 else dropped_frames,
        "audio_frame_seconds": audio_frame_seconds,
        "playback_dropped_frame_percent": dropped_frames * 100.0 / max(1.0, audio_frame_seconds),
        "jitter_max_ms": max((metric.get("jitter_max_ms", 0.0) for metric in metrics), default=0.0),
        "max_abs_final_drift_ppm": max((metric.get("max_abs_final_drift_ppm", 0.0) for metric in metrics), default=0.0),
        "max_abs_periodic_drift_ppm": max((metric.get("max_abs_periodic_drift_ppm", 0.0) for metric in metrics), default=0.0),
        "max_abs_final_ratio_delta_ppm": max((metric.get("max_abs_final_ratio_delta_ppm", 0.0) for metric in metrics), default=0.0),
        "max_abs_ratio_delta_ppm": max((metric.get("max_abs_ratio_delta_ppm", 0.0) for metric in metrics), default=0.0),
        "min_playback_depth_ms": min((metric.get("min_playback_depth_ms", 0.0) for metric in metrics), default=0.0),
        "max_abs_depth_delta_ms": max((metric.get("max_abs_depth_delta_ms", 0.0) for metric in metrics), default=0.0),
    }


def burst_badness(metrics):
    return (
        metrics.get("packet_loss_per_second", 0.0) / max(PLAYABLE_MAX_PACKET_LOSS_PER_SECOND, 0.001) +
        metrics.get("playback_ring_underrun_events_per_second", 0.0) / max(PLAYABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND, 0.001) +
        metrics.get("playback_ring_underruns_per_second", 0.0) / max(PLAYABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND, 0.001) +
        metrics.get("playback_dropped_frame_percent", 0.0) / max(PLAYABLE_MAX_DROPPED_FRAME_PERCENT, 0.001) +
        metrics.get("playback_ring_overruns", 0.0) * 10.0
    )


def isolated_burst_result(edge_summary, confirm_results, confirm_summary):
    if not edge_summary.get("playable", False):
        return None
    if confirm_summary.get("playable", False):
        return None
    if not confirm_results or any(is_infrastructure_failure(result) for result in confirm_results):
        return None
    if any(result.get("network_profile") == "wifi" for result in confirm_results):
        pass
    else:
        return None
    acceptable = [
        result for result in confirm_results
        if result["evaluation"].get("stable") or playable_accepts(result.get("metrics", {}), "wifi")
    ]
    if len(acceptable) < len(confirm_results) - 1:
        return None
    bad_results = [
        result for result in confirm_results
        if not (result["evaluation"].get("stable") or playable_accepts(result.get("metrics", {}), "wifi"))
    ]
    if len(bad_results) != 1:
        return None
    bad_score = burst_badness(bad_results[0].get("metrics", {}))
    other_scores = [burst_badness(result.get("metrics", {})) for result in acceptable]
    baseline = max(other_scores) if other_scores else 0.0
    if bad_score >= max(2.0, baseline * 3.0):
        return bad_results[0]
    return None


def should_retry_burst_confirm(edge_summary, confirm_results, confirm_summary):
    return isolated_burst_result(edge_summary, confirm_results, confirm_summary) is not None


def result_acceptable_for_burst_baseline(result):
    profile = result.get("network_profile", "unknown")
    metrics = result.get("metrics", {})
    return (
        result["evaluation"].get("stable")
        or playable_accepts(metrics, profile)
        or pressured_accepts(metrics, profile)
    )


def isolated_benchmark_burst_result(run_results):
    if len(run_results) < BENCHMARK_RUNS:
        return None
    if any(is_infrastructure_failure(result) for result in run_results):
        return None
    acceptable = [
        result for result in run_results
        if result_acceptable_for_burst_baseline(result)
    ]
    bad_results = [
        result for result in run_results
        if not result_acceptable_for_burst_baseline(result)
    ]
    if len(bad_results) != 1 or len(acceptable) < BENCHMARK_RUNS - 1:
        return None
    bad_score = burst_badness(bad_results[0].get("metrics", {}))
    other_scores = [burst_badness(result.get("metrics", {})) for result in acceptable]
    baseline = max(other_scores) if other_scores else 0.0
    if bad_score >= max(2.0, baseline * 3.0):
        return bad_results[0]
    return None


def should_probe_wider_deadband(candidate, run_results):
    metrics = aggregate_metrics(run_results)
    return (
        metrics["max_abs_final_drift_ppm"] > candidate.drift_deadband_ppm
        or metrics["max_abs_final_ratio_delta_ppm"] > 0.0
    )


def round_up_to_step(value, step):
    return int(math.ceil(value / step) * step)


def measured_deadband_candidate(candidate, run_results):
    metrics = aggregate_metrics(run_results)
    target = metrics["max_abs_final_drift_ppm"] + DRIFT_DEADBAND_CUSHION_PPM
    deadband = round_up_to_step(target, DRIFT_DEADBAND_STEP_PPM)
    deadband = max(candidate.drift_deadband_ppm + DRIFT_DEADBAND_STEP_PPM, deadband)
    deadband = min(deadband, DRIFT_DEADBAND_MAX_PPM)
    if deadband <= candidate.drift_deadband_ppm:
        return None
    return same_physical_candidate(candidate, "on", deadband)


def should_probe_drift_off(run_results):
    metrics = aggregate_metrics(run_results)
    return (
        metrics["max_abs_final_drift_ppm"] <= DRIFT_OFF_FINAL_PPM_LIMIT
        and metrics["max_abs_periodic_drift_ppm"] <= DRIFT_OFF_PERIODIC_PPM_LIMIT
        and metrics["max_abs_depth_delta_ms"] <= DRIFT_OFF_DEPTH_DELTA_MS_LIMIT
    )


def should_extend_drift_variant(base_summary, variant_probe_summary, variant_probe_results):
    if not variant_probe_summary["stable"]:
        return False
    variant_metrics = aggregate_metrics(variant_probe_results)
    base_metrics = base_summary.get("metrics", {})
    latency_better = (
        variant_probe_summary["latency_avg_ms"] + DRIFT_VARIANT_LATENCY_MARGIN_MS
        < base_summary["latency_avg_ms"]
    )
    correction_reduced = (
        variant_metrics.get("max_abs_final_ratio_delta_ppm", 0.0)
        < base_metrics.get("max_abs_final_ratio_delta_ppm", 0.0)
    )
    warnings_reduced = (
        len(variant_probe_summary.get("warnings", []))
        < len(base_summary.get("warnings", []))
    )
    return latency_better or correction_reduced or warnings_reduced


def aggregate_accepts(metrics, network_profile, aggressive=False):
    if metrics.get("elapsed_seconds", 0.0) <= 0.0:
        return False
    if metrics.get("playback_ring_overruns", 0.0) > 0.0:
        return False
    if metrics.get("min_playback_depth_ms", 0.0) < STABLE_HARD_PLAYBACK_DEPTH_MIN_MS:
        return False

    severity_stats = metrics.get("has_playback_severity_stats", False)
    if severity_stats:
        underruns_ok = (
            metrics.get("playback_ring_underrun_time_percent", 0.0) <= (0.05 if not aggressive else 0.15)
            and metrics.get("playback_ring_underrun_burst_max_ms", 0.0) <= (10.0 if not aggressive else 20.0)
        )
    else:
        underruns_ok = (
            metrics.get("playback_ring_underruns_per_second", 0.0) <= (
                AGGRESSIVE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND if aggressive else STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND)
            and metrics.get("playback_ring_underrun_events_per_second", 0.0) <= (
                AGGRESSIVE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND if aggressive else STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND)
            and (
                aggressive
                or metrics.get("max_playback_ring_underruns", 0.0) <= STABLE_MAX_PLAYBACK_UNDERRUNS_PER_RUN)
        )

    if aggressive:
        return (
            metrics.get("packet_loss_percent", 0.0) <= AGGRESSIVE_MAX_PACKET_LOSS_PERCENT
            and metrics.get("packet_loss_per_second", 0.0) <= AGGRESSIVE_MAX_PACKET_LOSS_PER_SECOND
            and underruns_ok
            and metrics.get("playback_dropped_frames_per_second", 0.0) <= AGGRESSIVE_MAX_DROPPED_FRAMES_PER_SECOND
            and metrics.get("jitter_max_ms", 0.0) <= AGGRESSIVE_MAX_JITTER_MS
        )

    return (
        metrics.get("packet_loss_percent", 0.0) <= STABLE_MAX_PACKET_LOSS_PERCENT
        and metrics.get("packet_loss_per_second", 0.0) <= STABLE_MAX_PACKET_LOSS_PER_SECOND
        and underruns_ok
        and metrics.get("playback_dropped_frames_per_second", 0.0) <= STABLE_MAX_DROPPED_FRAMES_PER_SECOND
    )


def playable_accepts(metrics, network_profile):
    if metrics.get("elapsed_seconds", 0.0) <= 0.0:
        return False
    if metrics.get("playback_ring_overruns", 0.0) > 0.0:
        return False
    if metrics.get("has_playback_severity_stats", False):
        underruns_ok = (
            metrics.get("playback_ring_underrun_time_percent", 0.0) <= 0.20
            and metrics.get("playback_ring_underrun_burst_max_ms", 0.0) <= 25.0
        )
    else:
        underruns_ok = (
            metrics.get("playback_ring_underruns_per_second", 0.0) <= PLAYABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND
            and metrics.get("playback_ring_underrun_events_per_second", 0.0) <= PLAYABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND
            and metrics.get("max_playback_ring_underruns", 0.0) <= PLAYABLE_MAX_PLAYBACK_UNDERRUNS_PER_RUN
        )
    return (
        metrics.get("min_playback_depth_ms", 0.0) >= PLAYABLE_MIN_PLAYBACK_DEPTH_MS
        and metrics.get("packet_loss_percent", 0.0) <= PLAYABLE_MAX_PACKET_LOSS_PERCENT
        and metrics.get("packet_loss_per_second", 0.0) <= PLAYABLE_MAX_PACKET_LOSS_PER_SECOND
        and underruns_ok
        and metrics.get("playback_dropped_frame_percent", 0.0) <= PLAYABLE_MAX_DROPPED_FRAME_PERCENT
        and metrics.get("jitter_max_ms", 0.0) <= PLAYABLE_MAX_JITTER_MS
    )


def pressured_accepts(metrics, network_profile):
    if metrics.get("elapsed_seconds", 0.0) <= 0.0:
        return False
    if metrics.get("playback_ring_overruns", 0.0) > 0.0:
        return False
    if metrics.get("min_playback_depth_ms", 0.0) < PLAYABLE_MIN_PLAYBACK_DEPTH_MS:
        return False
    if metrics.get("has_playback_severity_stats", False):
        underruns_ok = (
            metrics.get("playback_ring_underrun_time_percent", 0.0) <= 0.50
            and metrics.get("playback_ring_underrun_burst_max_ms", 0.0) <= 50.0
        )
    else:
        underruns_ok = (
            metrics.get("playback_ring_underruns_per_second", 0.0) <= PRESSURED_MAX_PLAYBACK_UNDERRUNS_PER_SECOND
            and metrics.get("playback_ring_underrun_events_per_second", 0.0) <= PRESSURED_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND
        )
    return (
        metrics.get("packet_loss_percent", 0.0) <= PRESSURED_MAX_PACKET_LOSS_PERCENT
        and metrics.get("packet_loss_per_second", 0.0) <= PRESSURED_MAX_PACKET_LOSS_PER_SECOND
        and underruns_ok
        and metrics.get("playback_dropped_frame_percent", 0.0) <= PRESSURED_MAX_DROPPED_FRAME_PERCENT
        and metrics.get("jitter_max_ms", 0.0) <= PRESSURED_MAX_JITTER_MS
    )


def experimental_accepts(metrics, network_profile):
    if metrics.get("elapsed_seconds", 0.0) <= 0.0:
        return False
    if metrics.get("playback_ring_overruns", 0.0) > 0.0:
        return False
    if metrics.get("min_playback_depth_ms", 0.0) < PLAYABLE_MIN_PLAYBACK_DEPTH_MS:
        return False
    return (
        metrics.get("packet_loss_percent", 0.0) <= EXPERIMENTAL_MAX_PACKET_LOSS_PERCENT
        and metrics.get("packet_loss_per_second", 0.0) <= EXPERIMENTAL_MAX_PACKET_LOSS_PER_SECOND
        and metrics.get("playback_dropped_frame_percent", 0.0) <= EXPERIMENTAL_MAX_DROPPED_FRAME_PERCENT
        and metrics.get("jitter_max_ms", 0.0) <= EXPERIMENTAL_MAX_JITTER_MS
    )


def practical_accepts(metrics, network_profile):
    return playable_accepts(metrics, network_profile) or pressured_accepts(metrics, network_profile)


def metric_score(metrics):
    return (
        metrics.get("packet_loss_per_second", 0.0) * 500.0 +
        metrics.get("playback_ring_underrun_events_per_second", 0.0) * 200.0 +
        metrics.get("playback_ring_underruns_per_second", 0.0) * 20.0 +
        metrics.get("playback_dropped_frames_per_second", 0.0) * 50.0 +
        metrics.get("playback_ring_overruns", 0.0) * 1000.0 +
        max(0.0, STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS - metrics.get("min_playback_depth_ms", 0.0)) * 50.0 +
        metrics.get("jitter_max_ms", 0.0) * 0.5 +
        metrics.get("elapsed_seconds", 0.0) * 0.0
    )


def practical_score(summary):
    metrics = summary.get("metrics", {})
    dropped_penalty = metrics.get("playback_dropped_frame_percent", 0.0) * 120.0
    underrun_penalty = min(metrics.get("playback_ring_underrun_events_per_second", 0.0), 80.0) * 0.03
    loss_penalty = metrics.get("packet_loss_per_second", 0.0) * 8.0
    return (
        summary.get("latency_avg_ms", 0.0) +
        loss_penalty +
        underrun_penalty +
        dropped_penalty +
        metrics.get("playback_ring_overruns", 0.0) * 1000.0 +
        max(0.0, STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS - metrics.get("min_playback_depth_ms", 0.0)) * 0.5 +
        max(0.0, metrics.get("jitter_max_ms", 0.0) - 60.0) * 0.05
    )


def practical_better(candidate_summary, previous_summary):
    if previous_summary is None:
        return True
    if candidate_summary.get("stable") and not previous_summary.get("stable"):
        return True
    if previous_summary.get("stable") and not candidate_summary.get("stable"):
        return False
    if not candidate_summary.get("playable", False):
        return False
    current_latency = candidate_summary.get("latency_avg_ms", 0.0)
    previous_latency = previous_summary.get("latency_avg_ms", 0.0)
    current_score = practical_score(candidate_summary)
    previous_score = practical_score(previous_summary)
    if current_latency + 2.0 < previous_latency and current_score <= previous_score + 3.0:
        return True
    if current_score + 4.0 < previous_score and current_latency <= previous_latency + 5.0:
        return True
    return False


def better_than(candidate_summary, previous_summary):
    if previous_summary is None:
        return True
    current = candidate_summary.get("metrics", {})
    previous = previous_summary.get("metrics", {})
    if candidate_summary.get("stable") and not previous_summary.get("stable"):
        return True
    return metric_score(current) + 1.0 < metric_score(previous)


def dominant_failures(summary):
    metrics = summary.get("metrics", {})
    reasons = set(summary.get("reasons", []))
    failures = []
    if metrics.get("playback_ring_overruns", 0.0) > 0.0:
        failures.append("overrun")
    if metrics.get("playback_dropped_frames_per_second", 0.0) > 0.0 or any("playback_dropped" in reason for reason in reasons):
        failures.append("dropped")
    if metrics.get("packet_loss_per_second", 0.0) > STABLE_MAX_PACKET_LOSS_PER_SECOND or any("packet_loss" in reason for reason in reasons):
        failures.append("packet_loss")
    if (
        metrics.get("playback_ring_underrun_events_per_second", 0.0) > STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND
        or metrics.get("playback_ring_underruns_per_second", 0.0) > STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND
        or any("playback_underrun" in reason for reason in reasons)
    ):
        failures.append("underrun")
    if metrics.get("min_playback_depth_ms", 0.0) < STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS:
        failures.append("depth")
    return failures


def add_candidate(target, output):
    if target is not None and valid_candidate(target):
        output.append(target)


def add_normal_audio_candidate(target, output):
    if target is not None and target.audio_buffer_size in NORMAL_AUDIO_BUFFER_VALUES:
        add_candidate(target, output)


def smart_next_candidates(candidate, summary, previous_summary=None):
    failures = dominant_failures(summary)
    next_prefill = next_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    previous_prefill = previous_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    next_frame = next_value(FRAME_VALUES, candidate.frame_size)
    previous_frame = previous_value(FRAME_VALUES, candidate.frame_size)
    next_audio = next_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    previous_audio = previous_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    candidates = []
    previous_candidate = None
    if previous_summary and previous_summary.get("profile"):
        previous_candidate = candidate_from_profile(previous_summary["profile"])

    if summary.get("stable"):
        add_candidate(with_prefill(candidate, previous_prefill), candidates)
        add_candidate(with_audio_buffer(candidate, previous_audio), candidates)
        add_candidate(with_frame_size(candidate, previous_frame), candidates)
        return candidates

    improved = better_than(summary, previous_summary)
    if "packet_loss" in failures and "underrun" not in failures and "dropped" not in failures:
        add_candidate(with_frame_size(candidate, next_frame), candidates)
        add_candidate(with_prefill(candidate, next_prefill), candidates)
        return candidates

    if "underrun" in failures or "depth" in failures:
        if not improved:
            if previous_candidate and previous_candidate.frame_size < candidate.frame_size:
                add_candidate(with_prefill(previous_candidate, next_prefill), candidates)
            elif previous_candidate and previous_candidate.audio_buffer_size < candidate.audio_buffer_size:
                add_candidate(with_prefill(previous_candidate, next_prefill), candidates)
            add_candidate(with_prefill(candidate, next_prefill), candidates)
            add_candidate(with_frame_size(candidate, next_frame), candidates)
        else:
            add_candidate(with_prefill(candidate, next_prefill), candidates)
        return candidates

    if "dropped" in failures or "overrun" in failures:
        if not improved and previous_candidate and previous_candidate.frame_size < candidate.frame_size:
            add_candidate(with_prefill(previous_candidate, next_prefill), candidates)
        else:
            add_candidate(with_prefill(candidate, next_prefill), candidates)
            add_candidate(with_audio_buffer(candidate, next_audio), candidates)
        return candidates

    add_candidate(with_prefill(candidate, next_prefill), candidates)
    add_candidate(with_frame_size(candidate, next_frame), candidates)
    return candidates


def benchmark_targeted_candidates(candidate, summary):
    metrics = summary.get("metrics", {})
    reasons = set(summary.get("reasons", []))
    failures = dominant_failures(summary)
    next_audio = benchmark_next_value(BENCHMARK_AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    previous_audio = benchmark_previous_value(BENCHMARK_AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    next_frame = benchmark_next_value(BENCHMARK_FRAME_VALUES, candidate.frame_size)
    previous_frame = benchmark_previous_value(BENCHMARK_FRAME_VALUES, candidate.frame_size)
    next_prefill = benchmark_next_value(BENCHMARK_PREFILL_VALUES, candidate.playback_prefill_frames)
    previous_prefill = benchmark_previous_value(BENCHMARK_PREFILL_VALUES, candidate.playback_prefill_frames)
    candidates = []

    def add(reason, target):
        if target is None or not benchmark_valid_candidate(target):
            return
        candidates.append((target, reason))

    capture_pressure = any("capture_underruns" in reason for reason in reasons)
    jitter_pressure = metrics.get("jitter_max_ms", 0.0) > PRESSURED_MAX_JITTER_MS
    if capture_pressure:
        add("capture underruns increased; test a larger audio callback buffer", benchmark_with_audio_buffer(candidate, next_audio))
    if "packet_loss" in failures or jitter_pressure:
        add("packet loss or jitter pressure; test a larger packet frame size", benchmark_with_frame_size(candidate, next_frame))
    if "underrun" in failures or "depth" in failures or "dropped" in failures or "overrun" in failures:
        add("playback cushion pressure; test a larger prefill", benchmark_with_prefill(candidate, next_prefill))
    if summary.get("stable"):
        if metrics.get("min_playback_depth_ms", 0.0) >= STABLE_TARGET_PLAYBACK_DEPTH_MIN_MS:
            add("stable with depth margin; test lower prefill boundary", benchmark_with_prefill(candidate, previous_prefill))
        add("stable; test smaller audio callback boundary", benchmark_with_audio_buffer(candidate, previous_audio))
        add("stable; test smaller packet frame boundary", benchmark_with_frame_size(candidate, previous_frame))
    if not candidates:
        add("general backoff; test more playback cushion", benchmark_with_prefill(candidate, next_prefill))
        add("general backoff; test lower packet rate", benchmark_with_frame_size(candidate, next_frame))
    return candidates


def queue_benchmark_candidates(candidate, summary, pending, queued_ids, completed_ids):
    queued = 0
    for next_candidate, reason in benchmark_targeted_candidates(candidate, summary):
        if next_candidate.id in queued_ids or next_candidate.id in completed_ids:
            continue
        pending.append((next_candidate, reason))
        queued_ids.add(next_candidate.id)
        queued += 1
        print_flush(
            f"[server] benchmark queued {next_candidate.id}: "
            f"{reason} from {candidate.id}")
    return queued


def local_neighbor_candidates(candidate):
    candidates = []
    previous_prefill = previous_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    next_prefill = next_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    previous_audio = previous_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    next_audio = next_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    previous_frame = previous_value(FRAME_VALUES, candidate.frame_size)
    next_frame = next_value(FRAME_VALUES, candidate.frame_size)
    add_candidate(with_prefill(candidate, previous_prefill), candidates)
    add_candidate(with_prefill(candidate, next_prefill), candidates)
    add_normal_audio_candidate(with_audio_buffer(candidate, previous_audio), candidates)
    add_normal_audio_candidate(with_audio_buffer(candidate, next_audio), candidates)
    add_candidate(with_frame_size(candidate, previous_frame), candidates)
    add_candidate(with_frame_size(candidate, next_frame), candidates)
    return candidates


def confirm_neighbor_candidates(candidate):
    candidates = []
    previous_prefill = previous_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    next_prefill = next_value(PREFILL_VALUES, candidate.playback_prefill_frames)
    previous_audio = previous_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    next_audio = next_value(AUDIO_BUFFER_VALUES, candidate.audio_buffer_size)
    previous_frame = previous_value(FRAME_VALUES, candidate.frame_size)
    add_candidate(with_prefill(candidate, previous_prefill), candidates)
    add_candidate(with_prefill(candidate, next_prefill), candidates)
    add_normal_audio_candidate(with_audio_buffer(candidate, previous_audio), candidates)
    add_normal_audio_candidate(with_audio_buffer(candidate, next_audio), candidates)
    add_candidate(with_frame_size(candidate, previous_frame), candidates)
    return candidates


def queue_local_neighbors(candidate, pending_candidates, queued_candidate_ids, candidate_stages):
    queued = 0
    for next_candidate in local_neighbor_candidates(candidate):
        if not can_queue_candidate(next_candidate, candidate_stages, queued_candidate_ids, allow_probe_retry=True):
            continue
        pending_candidates.append(next_candidate)
        queued_candidate_ids.add(next_candidate.id)
        queued += 1
        print_flush(
            f"[server] queued local tuning candidate: "
            f"{next_candidate.id} from {candidate.id}")
    return queued


def queue_confirm_neighbors(candidate, pending_candidates, queued_candidate_ids, candidate_stages):
    queued = 0
    for next_candidate in confirm_neighbor_candidates(candidate):
        if not can_queue_candidate(next_candidate, candidate_stages, queued_candidate_ids, allow_probe_retry=True):
            continue
        pending_candidates.append(next_candidate)
        queued_candidate_ids.add(next_candidate.id)
        queued += 1
        print_flush(
            f"[server] queued confirm-neighbor candidate: "
            f"{next_candidate.id} from {candidate.id}")
    return queued


def evaluate_candidate(base_logs, candidate, run_results, network_profile="unknown"):
    stable_runs = [result for result in run_results if result["evaluation"]["stable"]]
    infra_failures = [result for result in run_results if is_infrastructure_failure(result)]
    all_reasons = sorted({reason for result in run_results for reason in result["evaluation"]["reasons"]})
    all_warnings = sorted({warning for result in run_results for warning in result["evaluation"]["warnings"]})
    latency = [result["evaluation"]["latency_avg_ms"] for result in run_results if result["evaluation"]["latency_avg_ms"] > 0]
    metrics = aggregate_metrics(run_results)
    good_ratio = len(stable_runs) / len(run_results) if run_results else 0.0
    accepted = (
        len(infra_failures) == 0
        and good_ratio >= STABLE_MIN_GOOD_RUN_RATIO
        and aggregate_accepts(metrics, network_profile, aggressive=False)
    )
    clean_playable = (
        len(infra_failures) == 0
        and len(run_results) >= PLAYABLE_MIN_RUNS
        and playable_accepts(metrics, network_profile)
    )
    pressured_playable = (
        len(infra_failures) == 0
        and len(run_results) >= PLAYABLE_MIN_RUNS
        and not clean_playable
        and pressured_accepts(metrics, network_profile)
    )
    experimental = (
        len(infra_failures) == 0
        and len(run_results) >= PLAYABLE_MIN_RUNS
        and experimental_accepts(metrics, network_profile)
    )
    if accepted and len(stable_runs) != len(run_results):
        all_warnings.append(f"mostly_stable_runs({len(stable_runs)}/{len(run_results)})")
    if clean_playable and not accepted:
        all_warnings.append("practical_playable_not_strict_stable")
    if pressured_playable:
        all_warnings.append("pressured_playable_underruns_without_dropouts")
    if experimental and not (accepted or clean_playable or pressured_playable):
        all_warnings.append("experimental_audio_issues_expected")
    return {
        "candidate": candidate.id,
        "profile": candidate.profile(),
        "stable": accepted,
        "playable": accepted or clean_playable or pressured_playable,
        "pressured_playable": pressured_playable,
        "experimental": experimental,
        "stable_runs": len(stable_runs),
        "run_count": len(run_results),
        "reasons": all_reasons,
        "warnings": all_warnings,
        "latency_avg_ms": sum(latency) / len(latency) if latency else 0.0,
        "metrics": metrics,
    }


def powershell_quote(value):
    text = str(value)
    if not text:
        return '""'
    if any(char.isspace() for char in text) or any(char in text for char in ('"', "'", "&", "(", ")")):
        return '"' + text.replace('"', '`"') + '"'
    return text


def command_line(args):
    return " ".join(powershell_quote(arg) for arg in args)


def posix_command_line(args):
    return " ".join(shlex.quote(str(arg)) for arg in args)


def first_command_token(command_line, platform):
    if not command_line:
        return ""
    platform = (platform or "").lower()
    if platform.startswith("win"):
        text = command_line.strip()
        if not text:
            return ""
        if text[0] == '"':
            end = text.find('"', 1)
            return text[1:end] if end > 0 else text[1:]
        return text.split()[0] if text.split() else ""
    try:
        return shlex.split(command_line)[0]
    except ValueError:
        return command_line.split()[0] if command_line.split() else ""


def client_jam2_command(server_jam2, client_platform, client_row):
    uploaded_command = first_command_token(client_row.get("command_line", ""), client_platform)
    if uploaded_command:
        return uploaded_command
    platform = (client_platform or "").lower()
    if platform == "macos" or platform.startswith("darwin"):
        return "<repo>/build/jam2"
    if platform.startswith("linux"):
        return "<repo>/build/jam2"
    if platform.startswith("win"):
        return server_jam2
    return "<path-to-jam2>"


def command_line_for_platform(args, platform):
    platform = (platform or "").lower()
    if platform == "macos" or platform.startswith("darwin") or platform.startswith("linux"):
        return posix_command_line(args)
    return command_line(args)


def first_final_row_for_side(base_logs, candidate_id, side):
    side_dir = base_logs / candidate_id / side
    if not side_dir.exists():
        return {}
    for path in sorted(side_dir.glob("run_*/stats.csv")):
        rows = read_csv_rows(path)
        row = final_row(rows)
        if row:
            return row
    return {}


def command_block(base_logs, summary, context):
    if not summary or not summary.get("profile"):
        return []
    candidate = candidate_from_profile(summary["profile"])
    jam2 = context["jam2"]
    sample_rate = context["sample_rate"]
    server_audio_device = context["server_audio_device"]
    no_stun = context["no_stun"]
    client_row = first_final_row_for_side(base_logs, summary["candidate"], "client")
    client_audio_device = client_row.get("audio_device_id") or "<client-audio-device>"
    client_platform = client_row.get("platform") or "unknown"
    client_device_name = client_row.get("device_name") or "unknown"
    client_jam2 = client_jam2_command(jam2, client_platform, client_row)
    common_args = candidate.runtime_args()
    listen_args = [
        jam2,
        "listen",
        "--audio-device", str(server_audio_device),
        "--sample-rate", str(sample_rate),
    ]
    connect_args = [
        client_jam2,
        "connect",
        "<paste jam2://...>",
        "--audio-device", str(client_audio_device),
        "--sample-rate", str(sample_rate),
    ]
    if no_stun:
        listen_args.append("--no-stun")
        connect_args.append("--no-stun")
    listen_args.extend(common_args)
    connect_args.extend(common_args)
    return [
        f"  client_detected_platform={client_platform}",
        f"  client_detected_device={client_audio_device} ({client_device_name})",
        "  server_listen_command:",
        f"    {command_line(listen_args)}",
        "  client_connect_command:",
        f"    {command_line_for_platform(connect_args, client_platform)}",
    ]


def recommendation_lines(title, summary, base_logs, command_context):
    separator = "=" * 72
    lines = ["", separator, title, separator]
    if summary is None:
        lines.append("  none confirmed")
        return lines
    metrics = summary.get("metrics", {})
    lines.extend([
        f"  profile: {summary['candidate']}",
        f"  latency_avg_ms: {summary['latency_avg_ms']:.2f}",
        f"  stable: {str(summary.get('stable', False)).lower()}",
        f"  playable: {str(summary.get('playable', False)).lower()}",
        f"  pressured_playable: {str(summary.get('pressured_playable', False)).lower()}",
        f"  experimental: {str(summary.get('experimental', False)).lower()}",
        f"  stable_runs: {summary['stable_runs']}/{summary['run_count']}",
        f"  warnings: {','.join(summary.get('warnings', [])) or 'none'}",
        "",
        "  metrics:",
        f"    packet_loss: {metrics.get('packet_loss', 0.0):.0f}",
        f"    packet_loss_percent: {metrics.get('packet_loss_percent', 0.0):.4f}",
        f"    packet_loss_per_second: {metrics.get('packet_loss_per_second', 0.0):.3f}",
        f"    playback_dropped_frames: {metrics.get('playback_dropped_frames', 0.0):.0f}",
        f"    playback_dropped_frames_per_second: {metrics.get('playback_dropped_frames_per_second', 0.0):.3f}",
        f"    playback_dropped_frame_percent: {metrics.get('playback_dropped_frame_percent', 0.0):.4f}",
        f"    playback_underruns: {metrics.get('playback_ring_underruns', 0.0):.0f}",
        f"    playback_underruns_per_second: {metrics.get('playback_ring_underruns_per_second', 0.0):.3f}",
        f"    playback_underrun_events: {metrics.get('playback_ring_underrun_events', 0.0):.0f}",
        f"    playback_underrun_events_per_second: {metrics.get('playback_ring_underrun_events_per_second', 0.0):.3f}",
        f"    playback_underrun_time_percent: {metrics.get('playback_ring_underrun_time_percent', 0.0):.4f}",
        f"    playback_underrun_burst_max_ms: {metrics.get('playback_ring_underrun_burst_max_ms', 0.0):.2f}",
        f"    min_playback_depth_ms: {metrics.get('min_playback_depth_ms', 0.0):.2f}",
        f"    jitter_max_ms: {metrics.get('jitter_max_ms', 0.0):.2f}",
    ])
    if command_context is not None:
        lines.extend(["", "  commands:"])
        lines.extend(command_block(base_logs, summary, command_context))
    return lines


def print_recommendation_block(prefix, title, summary, base_logs, command_context):
    for line in recommendation_lines(title, summary, base_logs, command_context):
        print_flush(f"{prefix} {line}" if line else prefix)


def is_aggressive_candidate(summary):
    metrics = summary.get("metrics", {})
    allowed_reasons = {
        "server_playback_depth_low",
        "client_playback_depth_low",
        "server_playback_underrun",
        "client_playback_underrun",
        "server_packet_loss",
        "client_packet_loss",
        "server_playback_dropped",
        "client_playback_dropped",
    }
    event_rate = metrics.get("playback_ring_underrun_events_per_second", 0.0)
    if "playback_ring_underrun_events" not in metrics or metrics.get("playback_ring_underrun_events", 0.0) == 0.0:
        event_rate = metrics.get("playback_ring_underruns_per_second", 0.0) / max(1.0, float(summary.get("profile", {}).get("audio_buffer_size", 1)))
    return (
        bool(summary.get("profile"))
        and "playback_ring_underruns" in metrics
        and summary["run_count"] >= 1
        and all(reason in allowed_reasons for reason in summary.get("reasons", []))
        and metrics.get("playback_ring_overruns", 0.0) == 0.0
        and metrics.get("packet_loss_percent", 0.0) <= EXPERIMENTAL_MAX_PACKET_LOSS_PERCENT
        and metrics.get("packet_loss_per_second", 0.0) <= EXPERIMENTAL_MAX_PACKET_LOSS_PER_SECOND
        and metrics.get("playback_dropped_frame_percent", 0.0) <= EXPERIMENTAL_MAX_DROPPED_FRAME_PERCENT
        and metrics.get("min_playback_depth_ms", 0.0) >= AGGRESSIVE_MIN_PLAYBACK_DEPTH_MS
        and metrics.get("jitter_max_ms", 0.0) <= EXPERIMENTAL_MAX_JITTER_MS
    )


def choose_aggressive_summary(summaries):
    candidates = [summary for summary in summaries if is_aggressive_candidate(summary)]
    if not candidates:
        return None
    return min(candidates, key=lambda summary: (
        summary.get("profile", {}).get("audio_buffer_size", 999),
        summary.get("profile", {}).get("frame_size", 999),
        summary.get("profile", {}).get("playback_prefill_frames", 999999),
        summary["latency_avg_ms"],
        summary.get("metrics", {}).get("playback_dropped_frame_percent", 0.0),
        summary.get("metrics", {}).get("playback_ring_underrun_events_per_second", 0.0),
        summary.get("metrics", {}).get("playback_ring_underruns_per_second", 0.0),
        summary.get("metrics", {}).get("jitter_max_ms", 0.0)))


def choose_practical_summary(summaries):
    candidates = [
        summary for summary in summaries
        if summary.get("playable") and summary.get("run_count", 0) >= PLAYABLE_MIN_RUNS
        and not summary.get("confirm_failed", False)
    ]
    if not candidates:
        return None
    return min(candidates, key=lambda summary: (
        not summary.get("stable", False),
        practical_score(summary),
        summary["latency_avg_ms"],
        summary.get("metrics", {}).get("playback_ring_underrun_events_per_second", 0.0),
        summary.get("metrics", {}).get("playback_ring_underruns_per_second", 0.0)))


def write_summary(base_logs, rows, recommendation, run_csv_analysis, command_context=None):
    practical_recommendation = choose_practical_summary(rows)
    aggressive_recommendation = choose_aggressive_summary(rows)
    analysis_path = base_logs / "analysis.json"
    write_json(analysis_path, {
        "recommendation": recommendation,
        "practical_recommendation": practical_recommendation,
        "aggressive_recommendation": aggressive_recommendation,
        "candidates": rows,
    })
    text_path = base_logs / "recommendation.txt"
    lines = []
    lines.extend(recommendation_lines("Recommended practical starting profile", practical_recommendation, base_logs, command_context))
    lines.extend(recommendation_lines("Stable recommendation", recommendation, base_logs, command_context))
    lines.extend(recommendation_lines("Aggressive low-latency recommendation", aggressive_recommendation, base_logs, command_context))
    text_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if run_csv_analysis:
        try:
            combined, written, file_count = collect_matrix_csv(base_logs, side="all")
            print_flush(f"[server] combined {written} row(s) from {file_count} CSV file(s): {combined}")
            analysis_csv, analysis_rows = analyze_matrix_csv(
                combined,
                min_playback_depth_ms=STABLE_HARD_PLAYBACK_DEPTH_MIN_MS,
                min_runs=EDGE_RUNS,
                max_loss_percent=STABLE_MAX_PACKET_LOSS_PERCENT,
                max_loss_per_second=STABLE_MAX_PACKET_LOSS_PER_SECOND,
                max_playback_underruns_per_second=STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND,
                max_playback_underrun_events_per_second=STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND,
                max_playback_dropped_frames_per_second=STABLE_MAX_DROPPED_FRAMES_PER_SECOND,
                print_top=True)
            print_flush(f"[server] wrote CSV analysis for {len(analysis_rows)} profile(s): {analysis_csv}")
        except SystemExit as error:
            print_flush(f"[server] CSV analysis skipped: {error}")
    if run_csv_analysis:
        if practical_recommendation is not None:
            print_recommendation_block("[server]", "Recommended practical starting profile", practical_recommendation, base_logs, command_context)
        else:
            print_flush("[server] recommended practical starting profile: none confirmed")
        if recommendation is not None:
            print_recommendation_block("[server]", "Stable recommendation", recommendation, base_logs, command_context)
        else:
            print_flush("[server] stable recommendation: none confirmed")
        if aggressive_recommendation is not None:
            print_recommendation_block("[server]", "Aggressive low-latency recommendation", aggressive_recommendation, base_logs, command_context)
        else:
            print_flush("[server] aggressive low-latency recommendation: none")
    print_flush(f"[server] wrote adaptive analysis: {analysis_path}")


def benchmark_row(summary):
    profile = summary.get("profile", {})
    metrics = summary.get("metrics", {})
    return {
        "candidate": summary.get("candidate", ""),
        "phase": summary.get("phase", ""),
        "reason": summary.get("benchmark_reason", ""),
        "audio_buffer_size": profile.get("audio_buffer_size", ""),
        "frame_size": profile.get("frame_size", ""),
        "playback_prefill_frames": profile.get("playback_prefill_frames", ""),
        "playback_ring_frames": profile.get("playback_ring_frames", ""),
        "playback_max_frames": profile.get("playback_max_frames", ""),
        "stable": str(summary.get("stable", False)).lower(),
        "playable": str(summary.get("playable", False)).lower(),
        "pressured_playable": str(summary.get("pressured_playable", False)).lower(),
        "experimental": str(summary.get("experimental", False)).lower(),
        "stable_runs": summary.get("stable_runs", 0),
        "run_count": summary.get("run_count", 0),
        "benchmark_core_profile": str(summary.get("benchmark_core_profile", False)).lower(),
        "benchmark_attempt_count": summary.get("benchmark_attempt_count", summary.get("run_count", 0)),
        "benchmark_clean_runs": summary.get("benchmark_clean_runs", summary.get("run_count", 0)),
        "benchmark_discarded_burst_runs": ",".join(str(run) for run in summary.get("benchmark_discarded_burst_runs", [])) or "none",
        "benchmark_burst_retry_exhausted": str(summary.get("benchmark_burst_retry_exhausted", False)).lower(),
        "latency_avg_ms": f"{summary.get('latency_avg_ms', 0.0):.3f}",
        "packet_loss": f"{metrics.get('packet_loss', 0.0):.0f}",
        "packet_loss_percent": f"{metrics.get('packet_loss_percent', 0.0):.5f}",
        "packet_loss_per_second": f"{metrics.get('packet_loss_per_second', 0.0):.5f}",
        "playback_underruns": f"{metrics.get('playback_ring_underruns', 0.0):.0f}",
        "playback_underruns_per_second": f"{metrics.get('playback_ring_underruns_per_second', 0.0):.5f}",
        "playback_underrun_events": f"{metrics.get('playback_ring_underrun_events', 0.0):.0f}",
        "playback_underrun_events_per_second": f"{metrics.get('playback_ring_underrun_events_per_second', 0.0):.5f}",
        "playback_underrun_time_percent": f"{metrics.get('playback_ring_underrun_time_percent', 0.0):.5f}",
        "playback_underrun_burst_max_ms": f"{metrics.get('playback_ring_underrun_burst_max_ms', 0.0):.3f}",
        "playback_underrun_event_max_ms": f"{metrics.get('playback_ring_underrun_event_max_ms', 0.0):.3f}",
        "playback_overruns": f"{metrics.get('playback_ring_overruns', 0.0):.0f}",
        "playback_dropped_frames": f"{metrics.get('playback_dropped_frames', 0.0):.0f}",
        "playback_dropped_frames_per_second": f"{metrics.get('playback_dropped_frames_per_second', 0.0):.5f}",
        "playback_dropped_frame_percent": f"{metrics.get('playback_dropped_frame_percent', 0.0):.5f}",
        "playback_drop_event_max_ms": f"{metrics.get('playback_drop_event_max_ms', 0.0):.3f}",
        "min_playback_depth_ms": f"{metrics.get('min_playback_depth_ms', 0.0):.3f}",
        "jitter_max_ms": f"{metrics.get('jitter_max_ms', 0.0):.3f}",
        "max_abs_final_drift_ppm": f"{metrics.get('max_abs_final_drift_ppm', 0.0):.3f}",
        "max_abs_periodic_drift_ppm": f"{metrics.get('max_abs_periodic_drift_ppm', 0.0):.3f}",
        "max_abs_ratio_delta_ppm": f"{metrics.get('max_abs_ratio_delta_ppm', 0.0):.3f}",
        "max_abs_depth_delta_ms": f"{metrics.get('max_abs_depth_delta_ms', 0.0):.3f}",
        "reasons": ",".join(summary.get("reasons", [])) or "none",
        "warnings": ",".join(summary.get("warnings", [])) or "none",
    }


def write_benchmark_outputs(base_logs, summaries, command_context, run_csv_analysis=True):
    benchmark_json = base_logs / "benchmark.json"
    practical = choose_practical_summary(summaries)
    stable = choose_best_stable_summary(summaries)
    aggressive = choose_aggressive_summary(summaries)
    write_json(benchmark_json, {
        "lowest_measured_clean_profile": practical,
        "lowest_strictly_stable_profile": stable,
        "aggressive_low_latency_profile": aggressive,
        "candidates": summaries,
    })

    rows = [benchmark_row(summary) for summary in summaries]
    benchmark_csv = base_logs / "benchmark.csv"
    if rows:
        with open(benchmark_csv, "w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)

    lines = []
    lines.extend(recommendation_lines("Lowest measured clean profile", practical, base_logs, command_context))
    lines.extend(recommendation_lines("Lowest strictly stable profile", stable, base_logs, command_context))
    lines.extend(recommendation_lines("Aggressive low-latency measured profile", aggressive, base_logs, command_context))
    lines.append("")
    lines.append("=" * 72)
    lines.append("Measured Profiles")
    lines.append("=" * 72)
    for row in rows:
        lines.append(
            f"  {row['candidate']}: stable={row['stable']} playable={row['playable']} "
            f"runs={row['stable_runs']}/{row['run_count']} "
            f"attempts={row['benchmark_attempt_count']} clean_runs={row['benchmark_clean_runs']} "
            f"discarded_bursts={row['benchmark_discarded_burst_runs']} "
            f"lat_avg_ms={row['latency_avg_ms']} depth_min_ms={row['min_playback_depth_ms']} "
            f"loss_pct={row['packet_loss_percent']} underrun_eps={row['playback_underrun_events_per_second']} "
            f"underrun_time_pct={row['playback_underrun_time_percent']} "
            f"underrun_burst_max_ms={row['playback_underrun_burst_max_ms']} "
            f"dropped_pct={row['playback_dropped_frame_percent']} jitter_max_ms={row['jitter_max_ms']}"
        )
        lines.append(f"    reason: {row['reason'] or 'baseline'}")
        lines.append(f"    failures: {row['reasons']}")
    text_path = base_logs / "benchmark_summary.txt"
    text_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    if run_csv_analysis:
        try:
            combined, written, file_count = collect_matrix_csv(base_logs, side="all")
            print_flush(f"[server] combined {written} row(s) from {file_count} CSV file(s): {combined}")
            analysis_input = write_benchmark_clean_combined_csv(base_logs, combined, summaries)
            analysis_csv, analysis_rows = analyze_matrix_csv(
                analysis_input,
                min_playback_depth_ms=STABLE_HARD_PLAYBACK_DEPTH_MIN_MS,
                min_runs=BENCHMARK_RUNS,
                max_loss_percent=STABLE_MAX_PACKET_LOSS_PERCENT,
                max_loss_per_second=STABLE_MAX_PACKET_LOSS_PER_SECOND,
                max_playback_underruns_per_second=STABLE_MAX_PLAYBACK_UNDERRUNS_PER_SECOND,
                max_playback_underrun_events_per_second=STABLE_MAX_PLAYBACK_UNDERRUN_EVENTS_PER_SECOND,
                max_playback_dropped_frames_per_second=STABLE_MAX_DROPPED_FRAMES_PER_SECOND,
                print_top=True)
            print_flush(f"[server] wrote CSV analysis for {len(analysis_rows)} profile(s): {analysis_csv}")
        except SystemExit as error:
            print_flush(f"[server] CSV analysis skipped: {error}")

        if practical is not None:
            print_recommendation_block("[server]", "Lowest measured clean profile", practical, base_logs, command_context)
        else:
            print_flush("[server] lowest measured clean profile: none")
    print_flush(f"[server] wrote benchmark outputs: {benchmark_json}, {benchmark_csv}, {text_path}")


def write_benchmark_clean_combined_csv(base_logs, combined_path, summaries):
    discarded = set()
    for summary in summaries:
        candidate = summary.get("candidate", "")
        for run in summary.get("benchmark_discarded_burst_runs", []):
            discarded.add((candidate, str(run)))
    if not discarded:
        return combined_path
    clean_path = base_logs / "benchmark_clean_combined_stats.csv"
    with open(combined_path, "r", encoding="utf-8", newline="") as in_file:
        reader = csv.DictReader(in_file)
        if reader.fieldnames is None:
            return combined_path
        with open(clean_path, "w", encoding="utf-8", newline="") as out_file:
            writer = csv.DictWriter(out_file, fieldnames=reader.fieldnames, extrasaction="ignore")
            writer.writeheader()
            for row in reader:
                key = (row.get("matrix_test_id", ""), str(row.get("matrix_run", "")))
                if key in discarded:
                    continue
                writer.writerow(row)
    print_flush(
        f"[server] wrote benchmark clean combined CSV excluding "
        f"{len(discarded)} discarded burst run(s): {clean_path}")
    return clean_path


def run_candidate(
    jam2,
    candidate,
    stream_ms,
    base_logs,
    public_dir,
    audio_device,
    sample_rate,
    no_stun,
    run_count,
    phase,
    network_profile,
    server_network_type,
    start_run_index=1,
    wait_for_initial_client=False):
    results = []
    run_index = start_run_index
    accepted_runs = 0
    while accepted_runs < run_count:
        server_rc = run_listen(
            jam2,
            candidate,
            run_index,
            stream_ms,
            base_logs,
            public_dir,
            audio_device,
            sample_rate,
            no_stun,
            wait_for_initial_client and run_index == start_run_index,
            network_profile,
            server_network_type)
        client_uploaded = wait_for_client_upload(base_logs, candidate.id, run_index)
        run_client_network_type = client_network_type_for_run(base_logs, candidate.id, run_index)
        run_network_profile = effective_network_profile(
            network_profile,
            server_network_type,
            run_client_network_type)
        run_profile_note = network_profile_note(
            network_profile,
            run_network_profile,
            server_network_type,
            run_client_network_type)
        server = side_result(base_logs, candidate.id, "server", run_index)
        client = side_result(base_logs, candidate.id, "client", run_index)
        evaluation = evaluate_run(server, client, server_rc, client_uploaded, run_network_profile)
        probe_depth_failure = has_probe_depth_failure(server, client)
        metrics = run_metrics(server, client)
        print_flush(
            f"[server] {phase} evaluated {candidate.id} run {run_index}: "
            f"network_profile={run_network_profile}{run_profile_note} "
            f"stable={evaluation['stable']} reasons={','.join(evaluation['reasons']) or 'none'} "
            f"warnings={','.join(evaluation['warnings']) or 'none'}")
        results.append({
            "run_index": run_index,
            "server_rc": server_rc,
            "client_uploaded": client_uploaded,
            "network_profile": run_network_profile,
            "evaluation": evaluation,
            "probe_depth_failure": probe_depth_failure,
            "metrics": metrics,
        })
        if is_infrastructure_failure(results[-1]) and accepted_runs == 0 and run_index < start_run_index + INFRA_RETRIES:
            print_flush(f"[server] retrying {candidate.id} after infrastructure failure")
            run_index += 1
            results.clear()
            continue
        accepted_runs += 1
        run_index += 1
    return results


def choose_best_stable_summary(summaries):
    stable = [summary for summary in summaries if summary["stable"]]
    if not stable:
        return None
    return min(stable, key=lambda summary: summary["latency_avg_ms"])


def mark_confirm_failed(summaries, candidate_id):
    for summary in summaries:
        if summary.get("candidate") == candidate_id:
            summary["confirm_failed"] = True


def confirm_practical_candidate(
    jam2,
    candidate,
    summary,
    base_logs,
    public_dir,
    audio_device,
    sample_rate,
    no_stun,
    network_profile,
    server_network_type,
    summaries):
    confirm_results = run_candidate(
        jam2,
        candidate,
        CONFIRM_STREAM_MS,
        base_logs,
        public_dir,
        audio_device,
        sample_rate,
        no_stun,
        CONFIRM_RUNS,
        "confirm",
        network_profile,
        server_network_type,
        101)
    confirm_summary = evaluate_candidate(base_logs, candidate, confirm_results, network_profile)
    confirm_summary["confirmation"] = True
    confirm_summary["phase"] = "confirm"
    burst_result = isolated_burst_result(summary, confirm_results, confirm_summary)
    if burst_result is not None:
        print_flush(
            f"[server] retrying confirm for {candidate.id}: "
            f"discarding burst run {burst_result['run_index']} from recommendation averages")
        retry_confirm_results = run_candidate(
            jam2,
            candidate,
            CONFIRM_STREAM_MS,
            base_logs,
            public_dir,
            audio_device,
            sample_rate,
            no_stun,
            CONFIRM_BURST_RETRIES,
            "confirm-retry",
            network_profile,
            server_network_type,
            201)
        filtered_confirm_results = [
            result for result in confirm_results
            if result is not burst_result
        ] + retry_confirm_results
        retry_confirm_summary = evaluate_candidate(
            base_logs,
            candidate,
            filtered_confirm_results,
            network_profile)
        retry_confirm_summary["confirmation"] = True
        retry_confirm_summary["phase"] = "confirm_retry"
        retry_confirm_summary["warnings"].append(
            f"confirm_retried_after_isolated_wifi_burst(run_{burst_result['run_index']})")
        summaries.append(confirm_summary)
        confirm_summary = retry_confirm_summary
    return confirm_summary


def run_benchmark_candidate(
    args_ns,
    jam2,
    candidate,
    base_logs,
    public_dir,
    network_profile,
    server_network_type,
    wait_for_initial_client):
    clean_results = []
    discarded_burst_runs = []
    raw_results = []
    next_run_index = 1
    max_attempts = BENCHMARK_RUNS + BENCHMARK_BURST_RETRIES
    while len(clean_results) < BENCHMARK_RUNS and next_run_index <= max_attempts:
        result = run_candidate(
            jam2,
            candidate,
            BENCHMARK_STREAM_MS,
            base_logs,
            public_dir,
            args_ns.server_audio_device,
            args_ns.sample_rate,
            args_ns.no_stun,
            1,
            "benchmark",
            network_profile,
            server_network_type,
            start_run_index=next_run_index,
            wait_for_initial_client=wait_for_initial_client and next_run_index == 1)[0]
        raw_results.append(result)
        clean_results.append(result)
        burst_result = isolated_benchmark_burst_result(clean_results)
        if burst_result is not None:
            clean_results = [
                clean_result for clean_result in clean_results
                if clean_result is not burst_result
            ]
            discarded_burst_runs.append(burst_result["run_index"])
            if next_run_index < max_attempts:
                print_flush(
                    f"[server] benchmark retrying {candidate.id}: "
                    f"discarding isolated burst run {burst_result['run_index']} from aggregate")
            else:
                print_flush(
                    f"[server] benchmark exhausted burst retries for {candidate.id}: "
                    f"discarding isolated burst run {burst_result['run_index']} from aggregate")
        next_run_index = int(result["run_index"]) + 1

    summary = evaluate_candidate(base_logs, candidate, clean_results, network_profile)
    summary["benchmark_attempt_count"] = len(raw_results)
    summary["benchmark_clean_runs"] = len(clean_results)
    summary["benchmark_discarded_burst_runs"] = discarded_burst_runs
    summary["benchmark_burst_retry_exhausted"] = len(clean_results) < BENCHMARK_RUNS
    if discarded_burst_runs:
        summary["warnings"].append(
            "benchmark_retried_after_isolated_burst"
            f"(runs_{'-'.join(str(run) for run in discarded_burst_runs)})")
    if summary["benchmark_burst_retry_exhausted"]:
        summary["reasons"].append("benchmark_clean_run_count_low")
    return summary


def run_benchmark_mode(
    args_ns,
    jam2,
    base_logs,
    public_dir,
    network_profile,
    server_network_type,
    command_context):
    baseline_ids = {candidate.id for candidate in benchmark_baseline_candidates()}
    pending = [(candidate, "baseline spine") for candidate in benchmark_baseline_candidates()]
    queued_ids = {candidate.id for candidate, _ in pending}
    completed_ids = set()
    summaries = []
    baseline_failures = []

    while pending and len(completed_ids) < BENCHMARK_MAX_PROFILES:
        candidate, reason = pending.pop(0)
        queued_ids.discard(candidate.id)
        if candidate.id in completed_ids:
            continue
        print_flush(
            f"[server] benchmark testing {candidate.id} "
            f"({len(completed_ids) + 1}/{BENCHMARK_MAX_PROFILES}): {reason}")
        summary = run_benchmark_candidate(
            args_ns,
            jam2,
            candidate,
            base_logs,
            public_dir,
            network_profile,
            server_network_type,
            wait_for_initial_client=not summaries)
        summary["phase"] = "benchmark"
        summary["benchmark_reason"] = reason
        summary["benchmark_core_profile"] = candidate.id in baseline_ids
        summaries.append(summary)
        completed_ids.add(candidate.id)
        if candidate.id in baseline_ids and summary.get("benchmark_clean_runs", 0) < BENCHMARK_RUNS:
            baseline_failures.append(candidate.id)
        print_flush(
            f"[server] benchmark candidate {candidate.id}: stable={summary['stable']} "
            f"playable={summary.get('playable', False)} "
            f"runs={summary['stable_runs']}/{summary['run_count']} "
            f"attempts={summary.get('benchmark_attempt_count', summary['run_count'])} "
            f"discarded_bursts={','.join(str(run) for run in summary.get('benchmark_discarded_burst_runs', [])) or 'none'} "
            f"reasons={','.join(summary['reasons']) or 'none'} "
            f"warnings={','.join(summary['warnings']) or 'none'}")
        if len(completed_ids) + len(pending) < BENCHMARK_MAX_PROFILES:
            queue_benchmark_candidates(candidate, summary, pending, queued_ids, completed_ids)
        write_benchmark_outputs(base_logs, summaries, command_context, run_csv_analysis=False)

    write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
    write_benchmark_outputs(base_logs, summaries, command_context)
    if baseline_failures:
        print_flush(
            "[server] benchmark failed: core profile(s) did not collect "
            f"{BENCHMARK_RUNS} non-burst runs: {','.join(baseline_failures)}")
        return 1
    if choose_practical_summary(summaries) is None:
        print_flush("[server] benchmark found no clean measured profile under current limits")
        return 1
    return 0


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
    server_network_type = detect_network_type()
    client_network_type = "unknown"
    network_profile = effective_network_profile(args_ns.network_profile, server_network_type, client_network_type)
    command_context = {
        "jam2": str(jam2),
        "server_audio_device": args_ns.server_audio_device,
        "sample_rate": args_ns.sample_rate,
        "no_stun": args_ns.no_stun,
    }
    print_flush(f"[server] publishing {public_dir} on http://{args_ns.host}:{args_ns.port}/")
    print_flush(
        f"[server] network profile: {network_profile} "
        f"requested={args_ns.network_profile} server={server_network_type} client={client_network_type}"
        f"{network_profile_note(args_ns.network_profile, network_profile, server_network_type, client_network_type)}")
    try:
        if args_ns.benchmark:
            return run_benchmark_mode(
                args_ns,
                jam2,
                base_logs,
                public_dir,
                network_profile,
                server_network_type,
                command_context)

        ladder_candidates = list(candidate_ladder())
        ladder_index = 0
        pending_candidates = []
        candidate_stages = {}
        candidate_results = {}
        queued_candidate_ids = set()
        last_summary = None
        local_tuning = False
        local_best_summary = None
        local_expanded_candidate_ids = set()

        while pending_candidates or ladder_index < len(ladder_candidates):
            if local_tuning and not pending_candidates:
                practical_recommendation = choose_practical_summary(summaries)
                if practical_recommendation is not None:
                    confirm_candidate = candidate_from_profile(practical_recommendation["profile"])
                    print_flush(
                        f"[server] local tuning found practical candidate {confirm_candidate.id}; "
                        "running confirm before final recommendation")
                    confirm_summary = confirm_practical_candidate(
                        jam2,
                        confirm_candidate,
                        practical_recommendation,
                        base_logs,
                        public_dir,
                        args_ns.server_audio_device,
                        args_ns.sample_rate,
                        args_ns.no_stun,
                        network_profile,
                        server_network_type,
                        summaries)
                    confirm_summary["confirmation"] = True
                    summaries.append(confirm_summary)
                    candidate_stages[confirm_candidate.id] = "confirm"
                    if confirm_summary.get("stable") or confirm_summary.get("playable", False):
                        if confirm_summary["stable"]:
                            recommendation = confirm_summary
                        print_flush(
                            f"[server] practical confirmation accepted for {confirm_candidate.id}; "
                            "stopping after local tuning")
                        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
                        write_summary(base_logs, summaries, recommendation, True, command_context)
                        return 0
                    mark_confirm_failed(summaries, confirm_candidate.id)
                    print_flush(
                        f"[server] practical confirm failed for {confirm_candidate.id}; "
                        "continuing local search")
                    queue_confirm_neighbors(
                        confirm_candidate,
                        pending_candidates,
                        queued_candidate_ids,
                        candidate_stages)
                    if pending_candidates:
                        continue
                local_tuning = False

            if pending_candidates:
                base_candidate = pending_candidates.pop(0)
                queued_candidate_ids.discard(base_candidate.id)
            else:
                if local_tuning:
                    continue
                base_candidate = ladder_candidates[ladder_index]
                ladder_index += 1
            if candidate_stage_rank(candidate_stages.get(base_candidate.id, "")) >= candidate_stage_rank("edge"):
                continue

            candidates = [base_candidate]
            for candidate in candidates:
                existing_results = candidate_results.get(candidate.id, [])
                promote_probe_retry = bool(existing_results and candidate_stages.get(candidate.id) == "probe")
                baseline_candidate = is_low_latency_baseline(candidate)
                initial_run_count = EDGE_RUNS if baseline_candidate else PROBE_RUNS
                if existing_results:
                    probe_results = existing_results[:1]
                else:
                    probe_results = run_candidate(
                        jam2,
                        candidate,
                        SHORT_STREAM_MS,
                        base_logs,
                        public_dir,
                        args_ns.server_audio_device,
                        args_ns.sample_rate,
                        args_ns.no_stun,
                        initial_run_count,
                        "probe",
                        network_profile,
                        server_network_type,
                        wait_for_initial_client=not summaries)
                    candidate_results[candidate.id] = probe_results

                if (
                    not baseline_candidate
                    and not promote_probe_retry
                    and not is_promising_probe(probe_results[0])
                ):
                    summary = evaluate_candidate(base_logs, candidate, probe_results, network_profile)
                    summary["phase"] = "probe"
                    summaries.append(summary)
                    candidate_stages[candidate.id] = "probe"
                    print_flush(
                        f"[server] probe rejected {candidate.id}: "
                        f"reasons={','.join(summary['reasons']) or 'none'} "
                        f"warnings={','.join(summary['warnings']) or 'none'}")
                    if low_latency_baseline_complete(candidate_stages) and not queue_smart_candidates(
                        candidate,
                        summary,
                        last_summary,
                        pending_candidates,
                        queued_candidate_ids,
                        candidate_stages,
                        "probe"):
                        queue_prefill_jump(
                            candidate,
                            probe_results,
                            args_ns.sample_rate,
                            pending_candidates,
                            queued_candidate_ids,
                            candidate_stages,
                            "probe")
                    last_summary = summary
                    continue

                existing_results = candidate_results.get(candidate.id, [])
                existing_count = len(existing_results)
                if existing_count < EDGE_RUNS:
                    extra_runs = run_candidate(
                        jam2,
                        candidate,
                        SHORT_STREAM_MS,
                        base_logs,
                        public_dir,
                        args_ns.server_audio_device,
                        args_ns.sample_rate,
                        args_ns.no_stun,
                        EDGE_RUNS - existing_count,
                        "edge",
                        network_profile,
                        server_network_type,
                        start_run_index=existing_count + 1)
                    results = existing_results + extra_runs
                    candidate_results[candidate.id] = results
                else:
                    results = existing_results
                summary = evaluate_candidate(base_logs, candidate, results, network_profile)
                summary["phase"] = "edge"
                summaries.append(summary)
                candidate_stages[candidate.id] = "edge"
                print_flush(
                    f"[server] edge candidate {candidate.id}: stable={summary['stable']} "
                    f"playable={summary.get('playable', False)} "
                    f"runs={summary['stable_runs']}/{summary['run_count']} "
                    f"reasons={','.join(summary['reasons']) or 'none'} "
                    f"warnings={','.join(summary['warnings']) or 'none'}")
                if not summary["stable"]:
                    if summary.get("playable", False):
                        if practical_better(summary, local_best_summary):
                            local_best_summary = summary
                        if not low_latency_baseline_complete(candidate_stages):
                            print_flush(
                                f"[server] found practical profile {candidate.id}; "
                                "continuing low-latency baseline before local tuning")
                        else:
                            tuning_summary = local_best_summary or summary
                            tuning_candidate = candidate_from_profile(tuning_summary["profile"])
                            pending_candidates.clear()
                            queued_candidate_ids.clear()
                            if tuning_candidate.id not in local_expanded_candidate_ids:
                                local_expanded_candidate_ids.add(tuning_candidate.id)
                                queued = queue_local_neighbors(
                                    tuning_candidate,
                                    pending_candidates,
                                    queued_candidate_ids,
                                    candidate_stages)
                            else:
                                queued = 0
                            if not local_tuning:
                                print_flush(
                                    f"[server] low-latency baseline complete; "
                                    f"switching local tuning around {tuning_candidate.id}")
                            else:
                                print_flush(
                                    f"[server] improved practical profile {tuning_candidate.id}; "
                                    "refocusing local tuning around it")
                            local_tuning = True
                            if queued == 0:
                                print_flush(
                                    f"[server] no untested one-step neighbors remain for {tuning_candidate.id}")
                        last_summary = summary
                        continue
                    if local_tuning:
                        last_summary = summary
                        continue
                    if not low_latency_baseline_complete(candidate_stages):
                        last_summary = summary
                        continue
                    if local_best_summary is not None:
                        tuning_candidate = candidate_from_profile(local_best_summary["profile"])
                        pending_candidates.clear()
                        queued_candidate_ids.clear()
                        if tuning_candidate.id not in local_expanded_candidate_ids:
                            local_expanded_candidate_ids.add(tuning_candidate.id)
                            queued = queue_local_neighbors(
                                tuning_candidate,
                                pending_candidates,
                                queued_candidate_ids,
                                candidate_stages)
                        else:
                            queued = 0
                        print_flush(
                            f"[server] low-latency baseline complete; "
                            f"switching local tuning around {tuning_candidate.id}")
                        if queued == 0:
                            print_flush(
                                f"[server] no untested one-step neighbors remain for {tuning_candidate.id}")
                        local_tuning = True
                        last_summary = summary
                        continue
                    if not queue_smart_candidates(
                        candidate,
                        summary,
                        last_summary,
                        pending_candidates,
                        queued_candidate_ids,
                        candidate_stages,
                        "edge"):
                        queue_prefill_jump(
                            candidate,
                            results,
                            args_ns.sample_rate,
                            pending_candidates,
                            queued_candidate_ids,
                            candidate_stages,
                            "edge")
                    last_summary = summary
                if summary["stable"]:
                    if practical_better(summary, local_best_summary):
                        local_best_summary = summary
                    if not low_latency_baseline_complete(candidate_stages):
                        print_flush(
                            f"[server] stable profile {candidate.id}; "
                            "continuing low-latency baseline before confirmation")
                        last_summary = summary
                        continue
                    if baseline_candidate and local_best_summary is not None and not local_tuning:
                        tuning_candidate = candidate_from_profile(local_best_summary["profile"])
                        pending_candidates.clear()
                        queued_candidate_ids.clear()
                        if tuning_candidate.id not in local_expanded_candidate_ids:
                            local_expanded_candidate_ids.add(tuning_candidate.id)
                            queued = queue_local_neighbors(
                                tuning_candidate,
                                pending_candidates,
                                queued_candidate_ids,
                                candidate_stages)
                        else:
                            queued = 0
                        print_flush(
                            f"[server] low-latency baseline complete; "
                            f"switching local tuning around {tuning_candidate.id}")
                        if queued == 0:
                            print_flush(
                                f"[server] no untested one-step neighbors remain for {tuning_candidate.id}")
                        local_tuning = True
                        last_summary = summary
                        continue
                    stable_summaries = [summary]
                    tested_candidates = {candidate.id: candidate}
                    drift_candidates = []
                    if args_ns.drift_probes and should_probe_wider_deadband(base_candidate, results):
                        wider_deadband = measured_deadband_candidate(base_candidate, results)
                        if wider_deadband is not None:
                            drift_candidates.append(wider_deadband)
                            metrics = summary["metrics"]
                            print_flush(
                                f"[server] probing measured drift deadband {wider_deadband.drift_deadband_ppm} ppm "
                                f"for {base_candidate.id}: "
                                f"final_drift_ppm={metrics['max_abs_final_drift_ppm']:.1f} "
                                f"final_ratio_delta_ppm={metrics['max_abs_final_ratio_delta_ppm']:.1f}")
                    elif args_ns.drift_probes:
                        print_flush(
                            f"[server] skipping wider drift deadband for {base_candidate.id}: "
                            "db25 correction activity is already inside the measured deadband")
                    if not args_ns.drift_probes:
                        metrics = summary["metrics"]
                        print_flush(
                            f"[server] skipping drift probes for {base_candidate.id}: "
                            f"final_drift_ppm={metrics['max_abs_final_drift_ppm']:.1f} "
                            f"periodic_drift_ppm={metrics['max_abs_periodic_drift_ppm']:.1f} "
                            f"depth_delta_ms={metrics['max_abs_depth_delta_ms']:.2f}")

                    for variant in drift_candidates:
                        tested_candidates[variant.id] = variant
                        variant_probe_results = run_candidate(
                            jam2,
                            variant,
                            SHORT_STREAM_MS,
                            base_logs,
                            public_dir,
                            args_ns.server_audio_device,
                            args_ns.sample_rate,
                            args_ns.no_stun,
                            PROBE_RUNS,
                            "drift-probe",
                            network_profile,
                            server_network_type)
                        variant_probe_summary = evaluate_candidate(base_logs, variant, variant_probe_results, network_profile)
                        variant_probe_summary["phase"] = "drift_probe"
                        if should_extend_drift_variant(summary, variant_probe_summary, variant_probe_results):
                            variant_extra_results = run_candidate(
                                jam2,
                                variant,
                                SHORT_STREAM_MS,
                                base_logs,
                                public_dir,
                                args_ns.server_audio_device,
                                args_ns.sample_rate,
                                args_ns.no_stun,
                                EDGE_RUNS - PROBE_RUNS,
                                "drift-edge",
                                network_profile,
                                server_network_type,
                                start_run_index=PROBE_RUNS + 1)
                            variant_results = variant_probe_results + variant_extra_results
                            variant_summary = evaluate_candidate(base_logs, variant, variant_results, network_profile)
                            variant_summary["phase"] = "drift"
                        else:
                            variant_summary = variant_probe_summary
                            print_flush(
                                f"[server] drift variant not extended {variant.id}: "
                                f"stable={variant_summary['stable']} "
                                f"reasons={','.join(variant_summary['reasons']) or 'none'}")
                        summaries.append(variant_summary)
                        if variant_summary["stable"] and variant_summary["run_count"] >= EDGE_RUNS:
                            stable_summaries.append(variant_summary)
                    best_summary = choose_best_stable_summary(stable_summaries)
                    best_candidate = candidate
                    if best_summary is not None and best_summary["candidate"] in tested_candidates:
                        best_candidate = tested_candidates[best_summary["candidate"]]
                    confirm_summary = confirm_practical_candidate(
                        jam2,
                        best_candidate,
                        summary,
                        base_logs,
                        public_dir,
                        args_ns.server_audio_device,
                        args_ns.sample_rate,
                        args_ns.no_stun,
                        network_profile,
                        server_network_type,
                        summaries)
                    summaries.append(confirm_summary)
                    candidate_stages[best_candidate.id] = "confirm"
                    if confirm_summary["stable"]:
                        recommendation = confirm_summary
                        write_summary(base_logs, summaries, recommendation, True, command_context)
                        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
                        return 0
                    if confirm_summary.get("playable", False):
                        print_flush(
                            f"[server] practical confirmation accepted for {best_candidate.id}; "
                            "stopping without strict stable confirmation")
                        write_summary(base_logs, summaries, recommendation, True, command_context)
                        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
                        return 0
                    mark_confirm_failed(summaries, best_candidate.id)
                    local_tuning = True
                    if not queue_confirm_neighbors(
                        best_candidate,
                        pending_candidates,
                        queued_candidate_ids,
                        candidate_stages):
                        print_flush(
                            f"[server] no untested confirm neighbors remain for {best_candidate.id}")
                    last_summary = confirm_summary
                    print_flush(f"[server] confirmation failed for {best_candidate.id}; continuing backoff")
                    break
            write_summary(base_logs, summaries, recommendation, False, command_context)
        write_json(public_dir / "current.json", {"status": "all_done", "url": "", "client_args": []})
        write_summary(base_logs, summaries, recommendation, True, command_context)
        return 1
    finally:
        server.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
