#!/usr/bin/env python3

import argparse
import csv
import http.server
import io
import json
import shutil
import socketserver
import threading
import time
import urllib.parse
import uuid
import zipfile
from pathlib import Path

from jam2_audio_analysis import analyze_recording_dir
from jam2_benchmark_suite import benchmark_cases
from jam2_benchmark_control import new_suite_id, same_run, start_control_server
from jam2_harness import collect_side_csv, rewrite_jam_url_endpoint, start_listener
from jam2_metrics import combined_summary
from jam2_tooling import default_jam2_path, ensure_dir, fail, print_flush, repo_root, write_json


CLIENT_UPLOAD_TIMEOUT_S = 0


class RetryCaseRun(Exception):
    pass


def parse_args():
    parser = argparse.ArgumentParser(description="Run a static recorded Jam2 benchmark suite.")
    parser.add_argument("--jam2", default=str(default_jam2_path()))
    parser.add_argument("--server-audio-device", required=True)
    parser.add_argument("--sample-rate", required=True, type=int)
    parser.add_argument("--logs", default=str(Path(__file__).with_name("benchmark_logs")))
    parser.add_argument("--bind-http", default="", help="optional HTTP diagnostic bind for current.json; disabled by default")
    parser.add_argument("--bind-control", default="0.0.0.0:49000", help="TCP benchmark control bind endpoint")
    parser.add_argument("--audio-bind", default="0.0.0.0:49000", help="Jam2 UDP listen bind endpoint for benchmark sessions")
    parser.add_argument("--public-audio-host", default="", help="optional legacy override for the host in the published Jam2 URL")
    parser.add_argument("--client-upload-timeout-s", type=float, default=0.0, help="wait for client upload; 0 waits indefinitely")
    parser.add_argument("--post-listener-upload-grace-s", type=float, default=0.0, help="wait this long for client results after the server-side Jam2 process exits; 0 waits indefinitely")
    parser.add_argument("--case-retry-limit", type=int, default=3, help="retry a case this many times after client TCP disconnect before UDP completion; 0 retries indefinitely")
    parser.add_argument("--stream-ms", type=int, default=30000)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--signals", default="silence,tone-440,pulse-1s")
    parser.add_argument("--case", action="append", default=[], help="exact case id to run; repeat for multiple")
    parser.add_argument("--no-metronome-cases", action="store_true")
    parser.add_argument("--list-cases", action="store_true")
    parser.add_argument("--initial-client-timeout-s", type=float, default=0.0, help="wait for the first TCP benchmark client before publishing cases; 0 waits indefinitely")
    parser.add_argument("--finish-grace-s", type=float, default=300.0, help="keep control server alive after publishing all_done")
    parser.add_argument("--clean", action="store_true")
    return parser.parse_args()


class UploadHandler(http.server.SimpleHTTPRequestHandler):
    public_dir = None
    logs_dir = None
    done_acked = False
    control_state = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(self.public_dir), **kwargs)

    def log_message(self, fmt, *args):
        if self.command == "GET" and self.path == "/current.json":
            return
        print_flush("[http] " + (fmt % args))

    def do_POST(self):
        if self.path == "/done-ack":
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if length > 0:
                self.rfile.read(length)
            type(self).done_acked = True
            response = b'{"ok":true}\n'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
            print_flush("[server] received all_done ack from client")
            return
        if self.path != "/upload":
            self.send_error(404, "unknown upload endpoint")
            return
        self.send_error(410, "artifact uploads use the TCP control connection")


def extract_client_upload(logs_dir, control_state, payload):
    upload_dir = ensure_dir(Path(logs_dir) / "_uploads")
    zip_path = upload_dir / f"client_{int(time.time() * 1000)}.zip"
    zip_path.write_bytes(payload)
    with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
        names = archive.namelist()
        if "result.json" not in names:
            raise ValueError("missing result.json")
        for name in names:
            path = Path(name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"unsafe zip member: {name}")
        result = json.loads(archive.read("result.json").decode("utf-8"))
        case_id = result["case_id"]
        run_index = int(result["run_index"])
        identity = {
            "suite_id": result.get("suite_id", ""),
            "case_id": case_id,
            "run_index": run_index,
            "attempt_id": result.get("attempt_id", ""),
        }
        target = ensure_dir(Path(logs_dir) / case_id / "client" / f"run_{run_index:02d}")
        if control_state is not None and (control_state.active_case is None or not same_run(identity, control_state.active_case)):
            existing_result = target / "result.json"
            if existing_result.exists():
                existing = json.loads(existing_result.read_text(encoding="utf-8"))
                if same_run(identity, existing):
                    print_flush(f"[server] duplicate client artifacts already present for {case_id} run {run_index}")
                    return
            raise ValueError(
                "stale upload identity "
                f"{identity.get('suite_id','')}/{case_id}/{run_index}/{identity.get('attempt_id','')}")
        if target.exists():
            shutil.rmtree(target)
        ensure_dir(target)
        archive.extractall(target)
    print_flush(f"[server] received client artifacts for {case_id} run {run_index} over TCP control")


def serve_http(bind_http, public_dir, logs_dir, control_state=None):
    host, port_text = bind_http.rsplit(":", 1)
    UploadHandler.public_dir = public_dir
    UploadHandler.logs_dir = logs_dir
    UploadHandler.done_acked = False
    UploadHandler.control_state = control_state

    class ReusableThreadingTcpServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True

    server = ReusableThreadingTcpServer((host, int(port_text)), UploadHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def endpoint_port(endpoint):
    try:
        _, port_text = endpoint.rsplit(":", 1)
        return int(port_text)
    except ValueError:
        return None


def jam_url_fields(jam_url):
    parsed = urllib.parse.urlparse(jam_url)
    query = urllib.parse.parse_qs(parsed.query)
    endpoint = query.get("endpoint", [""])[0]
    session_id = query.get("session", [""])[0]
    session_key = query.get("key", [""])[0]
    audio_port = endpoint_port(endpoint)
    return {
        "endpoint": endpoint,
        "audio_port": audio_port,
        "session_id": session_id,
        "session_key": session_key,
    }


def wait_for_client_result(logs_dir, case_id, run_index, timeout_s=CLIENT_UPLOAD_TIMEOUT_S):
    target = Path(logs_dir) / case_id / "client" / f"run_{run_index:02d}" / "result.json"
    deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
    while True:
        if target.exists():
            return target
        if deadline is not None and time.monotonic() >= deadline:
            return None
        time.sleep(0.25)


def read_client_return_code(client_result_path):
    if not client_result_path:
        return None
    try:
        result = json.loads(Path(client_result_path).read_text(encoding="utf-8"))
        return int(result.get("return_code", -1))
    except (OSError, ValueError, json.JSONDecodeError):
        return -1


def wait_for_case_progress(logs_dir, case_id, run_index, listener, public_dir, current, timeout_s=CLIENT_UPLOAD_TIMEOUT_S):
    target = Path(logs_dir) / case_id / "client" / f"run_{run_index:02d}" / "result.json"
    deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
    connected = False
    while True:
        if target.exists():
            return target, connected, "client_upload"
        if not connected and listener.startup_payloads("connected"):
            connected = True
            current = dict(current)
            current["server_stage"] = "connected"
            write_json(Path(public_dir) / "current.json", current)
            print_flush(f"[server] client connected for {case_id} run {run_index}")
        if listener.poll() is not None:
            return None, connected, "listener_exit"
        if deadline is not None and time.monotonic() >= deadline:
            return None, connected, "timeout"
        time.sleep(0.25)


def wait_for_case_progress_control(logs_dir, case_id, run_index, listener, public_dir, current, control_state, timeout_s=CLIENT_UPLOAD_TIMEOUT_S):
    target = Path(logs_dir) / case_id / "client" / f"run_{run_index:02d}" / "result.json"
    deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
    connected = False
    accepted = False
    client_started = False
    client_finished = False
    printed_accept_wait = False
    printed_audio_wait = False
    upload_expected = False
    printed_upload_wait = False
    last_peer_connected = control_state.snapshot().get("peer_connected", False)
    while True:
        if target.exists():
            return target, connected, "client_upload", {
                "accepted": accepted,
                "client_started": client_started,
                "client_finished": client_finished,
            }
        snapshot = control_state.snapshot()
        peer_connected = snapshot.get("peer_connected", False)
        if peer_connected != last_peer_connected:
            if peer_connected:
                print_flush(f"[server] TCP benchmark client reconnected peer={snapshot.get('peer_id') or '-'}")
            else:
                if client_finished or upload_expected:
                    print_flush("[server] TCP benchmark client disconnected after Jam2 finished; waiting for reconnect and artifact upload")
                else:
                    print_flush("[server] TCP benchmark client disconnected before Jam2 finished; retrying this run after reconnect")
                    return None, connected, "control_disconnect_before_finish", {
                        "accepted": accepted,
                        "client_started": client_started,
                        "client_finished": client_finished,
                    }
            last_peer_connected = peer_connected
        if control_state.has_message("case.accept", current):
            if not accepted:
                print_flush(f"[server] client accepted {case_id} run {run_index}")
            accepted = True
        if control_state.has_message("case.started", current):
            if not client_started:
                print_flush(f"[server] client started Jam2 for {case_id} run {run_index}")
            client_started = True
        if control_state.has_message("case.finished", current):
            if not client_finished:
                print_flush(f"[server] client finished Jam2 for {case_id} run {run_index}; waiting for artifact upload")
            client_finished = True
            upload_expected = True
        if not accepted and not printed_accept_wait:
            print_flush(f"[server] waiting for client to accept {case_id} run {run_index}")
            printed_accept_wait = True
        if accepted and not connected and not printed_audio_wait:
            print_flush(f"[server] waiting for Jam2 UDP audio connection for {case_id} run {run_index}")
            printed_audio_wait = True
        if not connected and listener.startup_payloads("connected"):
            connected = True
            current = dict(current)
            current["server_stage"] = "connected"
            current["control_phase"] = control_state.snapshot().get("active_phase", "")
            write_json(Path(public_dir) / "current.json", current)
            print_flush(f"[server] Jam2 UDP audio connected for {case_id} run {run_index}")
        if upload_expected and not printed_upload_wait:
            print_flush(f"[server] waiting for TCP artifact upload for {case_id} run {run_index}")
            printed_upload_wait = True
        if listener.poll() is not None:
            if not upload_expected:
                print_flush(f"[server] server-side Jam2 finished for {case_id} run {run_index}; waiting for client finish/upload state")
            upload_expected = True
            if not printed_upload_wait:
                print_flush(f"[server] waiting for TCP artifact upload for {case_id} run {run_index}")
                printed_upload_wait = True
            return None, connected, "listener_exit", {
                "accepted": accepted,
                "client_started": client_started,
                "client_finished": client_finished,
            }
        if deadline is not None and time.monotonic() >= deadline:
            return None, connected, "timeout", {
                "accepted": accepted,
                "client_started": client_started,
                "client_finished": client_finished,
            }
        time.sleep(0.25)


def wait_for_done_ack(timeout_s, control_state=None):
    deadline = time.monotonic() + timeout_s if timeout_s and timeout_s > 0 else None
    while True:
        if UploadHandler.done_acked or (control_state is not None and control_state.done_acked):
            return True
        if deadline is not None and time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


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
    server_active_rate = metrics.get("server_active_sample_rate", 0.0)
    client_active_rate = metrics.get("client_active_sample_rate", 0.0)
    if server_active_rate and client_active_rate and abs(server_active_rate - client_active_rate) > 1.0:
        tags.append("active_sample_rate_mismatch")
    for side in ("server", "client"):
        requested = metrics.get(f"{side}_requested_sample_rate", 0.0)
        active = metrics.get(f"{side}_active_sample_rate", 0.0)
        if requested and active and abs(requested - active) > 1.0:
            tags.append(f"{side}_active_sample_rate_differs")
    if metrics.get("loss_percent_max", 0.0) > 0.05:
        tags.append("packet_loss_high")
    if metrics.get("playback_underrun_time_ms_total", 0.0) > 100.0:
        tags.append("underrun_high")
    if metrics.get("playback_dropped_frames_total", 0.0) > 0:
        tags.append("playback_dropped_frames")
    if metrics.get("missing_audio_frames_total", 0.0) > 0:
        tags.append("missing_audio_frames")
    if metrics.get("jitter_buffer_dropped_packets_total", 0.0) > 0:
        tags.append("jitter_buffer_dropped_packets")
    if metrics.get("jitter_buffer_dropped_frames_total", 0.0) > 0:
        tags.append("jitter_buffer_dropped_frames")
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
        "server_signal", "client_signal",
        "server_return_code", "client_return_code",
        "server_requested_sample_rate", "server_active_sample_rate",
        "client_requested_sample_rate", "client_active_sample_rate",
        "audio_buffer_size", "frame_size", "playback_prefill_frames",
        "playback_ring_frames", "playback_max_frames", "playout_delay_frames",
        "adaptive_playback_cushion", "adaptive_playback_target_frames",
        "adaptive_playback_min_frames", "adaptive_playback_max_frames",
        "jitter_buffer_frames", "jitter_buffer_max_frames",
        "loss_percent_max", "jitter_max_ms", "rtt_max_ms",
        "playback_underrun_time_ms_total", "playback_underrun_burst_max_ms",
        "playback_dropped_frames_total", "missing_audio_frames_total",
        "late_audio_frames_total", "drift_abs_ppm_max",
        "adaptive_raise_events_total", "adaptive_burst_events_total",
        "jitter_buffer_released_packets_total", "jitter_buffer_dropped_packets_total",
        "jitter_buffer_dropped_frames_total", "jitter_buffer_depth_max_frames",
        "metronome_beat_delta_abs_max",
        "server_csv_path", "client_csv_path",
    ]
    with open(logs_dir / "benchmark_results.csv", "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for result in results:
            combined = result.get("metrics", {}).get("combined", {})
            profile_values = result.get("profile_values", {})
            writer.writerow({
                "case_id": result.get("case_id", ""),
                "run_index": result.get("run_index", ""),
                "profile": result.get("profile", ""),
                "signal": result.get("signal", ""),
                "server_signal": result.get("server_signal", ""),
                "client_signal": result.get("client_signal", ""),
                "verdict": result.get("verdict", ""),
                "tags": ";".join(result.get("tags", [])),
                "server_return_code": result.get("server_return_code", ""),
                "client_return_code": result.get("client_return_code", ""),
                "server_csv_path": result.get("server_csv_path", ""),
                "client_csv_path": result.get("client_csv_path", ""),
                **profile_values,
                **combined,
            })
    lines = []
    lines.append("Jam2 benchmark summary")
    lines.append("")
    lines.append(f"{'case':44} {'profile':28} {'signal':22} {'S':9} {'C':9} {'run':>3} {'verdict':8} tags")
    lines.append("-" * 138)
    for result in results:
        lines.append(
            f"{result.get('case_id','')[:44]:44} "
            f"{result.get('profile','')[:28]:28} "
            f"{result.get('signal','')[:22]:22} "
            f"{result.get('server_signal','')[:9]:9} "
            f"{result.get('client_signal','')[:9]:9} "
            f"{result.get('run_index', 0):>3} "
            f"{result.get('verdict',''):8} "
            f"{','.join(result.get('tags', [])) or '-'}")
    (logs_dir / "benchmark_summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_one_case(jam2, audio_device, sample_rate, logs_dir, public_dir, case, run_index, audio_bind, public_audio_host, client_upload_timeout_s, post_listener_upload_grace_s, suite_id, control_state=None):
    output_dir = Path(logs_dir) / case.case_id / "server" / f"run_{run_index:02d}"
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir = ensure_dir(output_dir)
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
            "--test-input", case.server_signal if case.server_signal != "metronome-only" else "silence",
        ] + list(case.server_args),
        bind=audio_bind,
        wait_ms=0)
    startup = listener.wait_for_startup("waiting", 10.0)
    connection_url = startup.get("connection_url") if startup else ""
    if not connection_url and startup:
        connection_url = startup.get("url", "")
    if not connection_url:
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
    startup_endpoint = startup.get("local_endpoint", "")
    if public_audio_host:
        _, _, port_text = startup_endpoint.rpartition(":")
        connection_url = rewrite_jam_url_endpoint(connection_url, (public_audio_host, int(port_text)))
    url_fields = jam_url_fields(connection_url)
    current = {
        "status": "running",
        "suite_id": suite_id,
        "attempt_id": uuid.uuid4().hex,
        "published_at_ms": int(time.time() * 1000),
        "case_id": case.case_id,
        "run_index": run_index,
        "url": connection_url,
        "audio_port": url_fields["audio_port"],
        "session_id": url_fields["session_id"],
        "session_key": url_fields["session_key"],
        "signal": case.signal,
        "server_signal": case.server_signal,
        "client_signal": case.client_signal,
        "stream_ms": case.stream_ms,
        "profile": case.profile.metadata(),
        "server_args": list(case.server_args),
        "case_client_args": list(case.client_args),
        "client_args": case.profile.args(case.stream_ms) + list(case.client_args),
    }
    write_json(Path(public_dir) / "current.json", current)
    control_start = control_state.snapshot() if control_state is not None else {}
    if control_state is not None:
        control_state.publish_case(current)
    print_flush(f"[server] offered {case.case_id} run {run_index} url={connection_url}")
    if client_upload_timeout_s and client_upload_timeout_s > 0:
        print_flush(f"[server] active-case timeout is {client_upload_timeout_s:g}s")
    else:
        print_flush("[server] active-case timeout disabled")
    if control_state is not None:
        client_result_path, connected, progress_reason, control_progress = wait_for_case_progress_control(
            logs_dir,
            case.case_id,
            run_index,
            listener,
            public_dir,
            current,
            control_state,
            client_upload_timeout_s)
    else:
        client_result_path, connected, progress_reason = wait_for_case_progress(
            logs_dir,
            case.case_id,
            run_index,
            listener,
            public_dir,
            current,
            client_upload_timeout_s)
        control_progress = {}
    client_rc = read_client_return_code(client_result_path)
    if client_result_path and client_rc != 0 and listener.poll() is None:
        print_flush(
            f"[server] client uploaded failure before stream completion "
            f"for {case.case_id} run {run_index}; stopping listener")
        listener.terminate()
        server_rc = listener.poll()
    elif client_result_path:
        server_rc = listener.wait(timeout=max(1.0, case.stream_ms / 1000.0 + 10.0))
        if server_rc is None:
            listener.terminate()
            server_rc = listener.poll()
    else:
        if progress_reason == "control_disconnect_before_finish":
            if listener.poll() is None:
                listener.terminate()
            if control_state is not None:
                control_state.clear_case()
            raise RetryCaseRun()
        if progress_reason == "listener_exit":
            if post_listener_upload_grace_s and post_listener_upload_grace_s > 0:
                print_flush(
                    f"[server] server-side case completed; waiting up to "
                    f"{post_listener_upload_grace_s:g}s for TCP artifact upload for {case.case_id} run {run_index}")
            else:
                print_flush(
                    f"[server] server-side case completed; waiting indefinitely for TCP artifact upload "
                    f"for {case.case_id} run {run_index}")
        elif progress_reason == "timeout":
            if not control_progress.get("accepted", False):
                print_flush(f"[server] timed out waiting for client accept for {case.case_id} run {run_index}")
            elif not control_progress.get("client_started", False):
                print_flush(f"[server] timed out waiting for client Jam2 start for {case.case_id} run {run_index}")
            elif not connected:
                print_flush(f"[server] timed out waiting for Jam2 UDP audio connection for {case.case_id} run {run_index}")
            else:
                print_flush(f"[server] timed out waiting for TCP artifact upload for {case.case_id} run {run_index}")
        if listener.poll() is None:
            listener.terminate()
        server_rc = listener.poll()
        if progress_reason == "listener_exit":
            client_result_path = wait_for_client_result(
                logs_dir,
                case.case_id,
                run_index,
                post_listener_upload_grace_s)
    server_csv = collect_side_csv(paths, listener)
    server_analysis = analyze_recording_dir(
        recording_dir,
        case.signal,
        local_signal=case.server_signal,
        remote_signal=case.client_signal)
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
        "server_args": list(case.server_args),
        "client_args": list(case.client_args),
        "signal": case.signal,
        "server_signal": case.server_signal,
        "client_signal": case.client_signal,
        "server_return_code": server_rc,
        "client_return_code": client_rc,
        "server_csv_path": str(server_csv) if server_csv else "",
        "client_csv_path": str(client_csv) if client_csv and client_csv.exists() else "",
        "metrics": metrics,
        "server_recording_analysis": server_analysis,
        "client_recording_analysis": client_analysis,
        "client_artifacts_received": client_result_path is not None,
        "client_connected": connected,
        "suite_id": suite_id,
        "attempt_id": current["attempt_id"],
        "control": control_state.snapshot() if control_state is not None else {},
        "client_accepted": control_progress.get("accepted", False),
        "client_started": control_progress.get("client_started", False),
        "client_finished": control_progress.get("client_finished", False),
    }
    tags = []
    if control_state is not None:
        snapshot = control_state.snapshot()
        if snapshot.get("disconnect_count", 0) > control_start.get("disconnect_count", 0):
            tags.append("control_disconnect")
        if snapshot.get("reconnect_count", 0) > control_start.get("reconnect_count", 0):
            tags.append("client_reconnected")
        if not control_progress.get("accepted", False):
            tags.append("client_accept_missing")
        if not control_progress.get("client_started", False):
            tags.append("client_start_missing")
    if not client_result_path:
        tags.append("client_upload_missing")
    if tags:
        result["tags"] = tags
    result = evaluate_result(result)
    write_json(output_dir / "result.json", result)
    if control_state is not None:
        control_state.clear_case()
    print_flush(f"[server] {case.case_id} run {run_index}: {result['verdict']} {','.join(result['tags']) or '-'}")
    return result


def retry_exhausted_result(case, run_index, suite_id):
    return evaluate_result({
        "case_id": case.case_id,
        "run_index": run_index,
        "profile": case.profile.name,
        "profile_values": case.profile.metadata(),
        "server_args": list(case.server_args),
        "client_args": list(case.client_args),
        "signal": case.signal,
        "server_signal": case.server_signal,
        "client_signal": case.client_signal,
        "server_return_code": -1,
        "client_return_code": -1,
        "suite_id": suite_id,
        "tags": ["client_disconnected_before_finish", "case_retry_exhausted"],
    })


def main():
    args = parse_args()
    signals = tuple(item.strip() for item in args.signals.split(",") if item.strip())
    cases = benchmark_cases(
        signals=signals,
        include_metronome=not args.no_metronome_cases,
        stream_ms=args.stream_ms,
        repeats=args.repeats)
    if args.case:
        requested = set(args.case)
        cases = [case for case in cases if case.case_id in requested]
        found = {case.case_id for case in cases}
        missing = sorted(requested - found)
        if missing:
            return fail("unknown benchmark case id(s): " + ", ".join(missing))
    if args.list_cases:
        for case in cases:
            print(
                f"{case.case_id} profile={case.profile.name} signal={case.signal} "
                f"server_signal={case.server_signal} client_signal={case.client_signal} repeats={case.repeats}")
        return 0
    jam2 = Path(args.jam2)
    if not jam2.exists():
        return fail(f"jam2 executable not found: {jam2}")
    public_audio_host = args.public_audio_host.strip()
    audio_bind_port = endpoint_port(args.audio_bind)
    if args.public_audio_host.strip() and audio_bind_port == 0:
        print_flush(
            "[server] warning: --public-audio-host with --audio-bind port 0 publishes a random UDP port; "
            "public-IP runs usually need a fixed forwarded UDP port such as --audio-bind 0.0.0.0:49000")
    print_flush(f"[server] audio bind={args.audio_bind}")
    suite_id = new_suite_id()
    print_flush(f"[server] benchmark suite id={suite_id}")
    logs_dir = Path(args.logs)
    if args.clean and logs_dir.exists():
        shutil.rmtree(logs_dir)
    logs_dir = ensure_dir(logs_dir)
    public_dir = ensure_dir(logs_dir / "public")
    write_json(public_dir / "current.json", {"status": "starting", "suite_id": suite_id})
    control_state_holder = {}

    def handle_control_upload(message, payload):
        extract_client_upload(logs_dir, control_state_holder["state"], payload)

    control_server, control_state = start_control_server(
        args.bind_control,
        suite_id,
        print_flush,
        upload_callback=handle_control_upload)
    control_state_holder["state"] = control_state
    print_flush(f"[server] TCP control bind={args.bind_control}")
    server = serve_http(args.bind_http, public_dir, logs_dir, control_state) if args.bind_http else None
    if args.bind_http:
        print_flush(f"[server] HTTP diagnostics bind={args.bind_http}")
    results = []
    try:
        if args.initial_client_timeout_s and args.initial_client_timeout_s > 0:
            print_flush(
                f"[server] waiting up to {args.initial_client_timeout_s:g}s for TCP benchmark client "
                f"before publishing cases")
        else:
            print_flush("[server] waiting for TCP benchmark client before publishing cases")
        if not control_state.wait_for_peer(args.initial_client_timeout_s):
            return fail("timed out waiting for TCP benchmark client before publishing cases")
        print_flush(f"[server] TCP benchmark client ready peer={control_state.snapshot().get('peer_id') or '-'}")
        for case in cases:
            for run_index in range(1, case.repeats + 1):
                retry_count = 0
                while True:
                    if not control_state.snapshot().get("peer_connected", False):
                        print_flush("[server] waiting for TCP benchmark client to reconnect before next case")
                        control_state.wait_for_peer(0.0)
                        print_flush(f"[server] TCP benchmark client ready peer={control_state.snapshot().get('peer_id') or '-'}")
                    try:
                        results.append(run_one_case(
                            jam2, args.server_audio_device, args.sample_rate,
                            logs_dir, public_dir, case, run_index,
                            args.audio_bind, public_audio_host, args.client_upload_timeout_s,
                            args.post_listener_upload_grace_s, suite_id, control_state))
                        break
                    except RetryCaseRun:
                        retry_count += 1
                        if args.case_retry_limit > 0 and retry_count > args.case_retry_limit:
                            print_flush(
                                f"[server] retry limit exceeded for {case.case_id} run {run_index} "
                                "after client disconnected before Jam2 finished")
                            results.append(retry_exhausted_result(case, run_index, suite_id))
                            break
                        print_flush(
                            f"[server] will retry {case.case_id} run {run_index} after TCP client reconnect "
                            f"(retry {retry_count})")
                write_outputs(logs_dir, results)
        write_json(public_dir / "current.json", {"status": "all_done", "suite_id": suite_id})
        control_state.mark_all_done()
        write_outputs(logs_dir, results)
        if args.finish_grace_s > 0:
            print_flush(f"[server] published all_done; waiting up to {args.finish_grace_s:g}s for client ack")
            if not wait_for_done_ack(args.finish_grace_s, control_state):
                print_flush("[server] all_done ack not received before finish grace expired")
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        control_server.shutdown()
        control_server.server_close()
    print_flush(f"[server] wrote {logs_dir / 'benchmark_summary.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
