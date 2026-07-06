#!/usr/bin/env python3

import json
import subprocess
import threading
import time
from pathlib import Path

from jam2_tooling import copy_final_csv, ensure_dir, repo_root


def parse_endpoint(value):
    host, port = value.rsplit(":", 1)
    return host, int(port)


def rewrite_jam_url_endpoint(url, endpoint):
    prefix = "jam2://v1?"
    if not url.startswith(prefix):
        raise ValueError(f"unsupported Jam2 URL: {url}")
    query = url[len(prefix):]
    parts = []
    replaced = False
    for item in query.split("&"):
        key, _, value = item.partition("=")
        if key == "endpoint":
            parts.append(f"endpoint={endpoint[0]}:{endpoint[1]}")
            replaced = True
        else:
            parts.append(item)
    if not replaced:
        parts.insert(0, f"endpoint={endpoint[0]}:{endpoint[1]}")
    return prefix + "&".join(parts)


class ManagedProcess:
    def __init__(self, args, cwd, stdout_path, stderr_path, stdin_pipe=False):
        self.args = [str(arg) for arg in args]
        self.cwd = str(cwd)
        self.stdout_path = Path(stdout_path)
        self.stderr_path = Path(stderr_path)
        self.process = None
        self._stdout_handle = None
        self._stderr_handle = None
        self._lines = []
        self._lock = threading.Lock()
        self._thread = None
        self._stdin_pipe = stdin_pipe

    def start(self):
        self.stdout_path.parent.mkdir(parents=True, exist_ok=True)
        self.stderr_path.parent.mkdir(parents=True, exist_ok=True)
        self._stdout_handle = open(self.stdout_path, "w", encoding="utf-8", newline="")
        self._stderr_handle = open(self.stderr_path, "w", encoding="utf-8", newline="")
        self.process = subprocess.Popen(
            self.args,
            cwd=self.cwd,
            stdout=subprocess.PIPE,
            stderr=self._stderr_handle,
            stdin=subprocess.PIPE if self._stdin_pipe else subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self._thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._thread.start()
        return self

    def wait(self, timeout=None):
        if self.process is None:
            return None
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        finally:
            if self.process.poll() is not None:
                self._close_handles()

    def terminate(self, grace_s=5.0):
        if self.process is None or self.process.poll() is not None:
            self._close_handles()
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=grace_s)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=grace_s)
        self._close_handles()

    def poll(self):
        return self.process.poll() if self.process is not None else None

    def send_line(self, line):
        if self.process is None or self.process.stdin is None or self.process.poll() is not None:
            return False
        try:
            self.process.stdin.write(line.rstrip("\r\n") + "\n")
            self.process.stdin.flush()
            return True
        except OSError:
            return False

    def lines(self):
        with self._lock:
            return list(self._lines)

    def wait_for_startup(self, stage, timeout_s):
        deadline = time.monotonic() + timeout_s
        seen = 0
        while time.monotonic() < deadline:
            lines = self.lines()
            for line in lines[seen:]:
                payload = self._startup_json(line)
                if payload and payload.get("stage") == stage:
                    return payload
            seen = len(lines)
            if self.poll() is not None:
                break
            time.sleep(0.05)
        return None

    def _read_stdout(self):
        try:
            for line in self.process.stdout:
                self._stdout_handle.write(line)
                self._stdout_handle.flush()
                with self._lock:
                    self._lines.append(line.rstrip("\r\n"))
        finally:
            if self.process.stdout:
                self.process.stdout.close()

    def _startup_json(self, line):
        line = line.strip()
        if not line.startswith("{"):
            return None
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            return None
        if payload.get("event") != "startup":
            return None
        return payload

    def _close_handles(self):
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        if self._stdout_handle:
            self._stdout_handle.close()
            self._stdout_handle = None
        if self._stderr_handle:
            self._stderr_handle.close()
            self._stderr_handle = None
        if self.process is not None and self.process.stdin is not None:
            try:
                self.process.stdin.close()
            except OSError:
                pass


def side_paths(base_dir, side):
    side_dir = ensure_dir(Path(base_dir) / side)
    return {
        "dir": side_dir,
        "csv_raw": ensure_dir(side_dir / "csv_raw"),
        "stdout": side_dir / "stdout.txt",
        "stderr": side_dir / "stderr.txt",
    }


def start_listener(jam2, audio_device, sample_rate, profile, stream_ms, output_dir, extra_args=None, stdin_pipe=False):
    paths = side_paths(output_dir, "server")
    args = [
        jam2,
        "listen",
        "--bind", "127.0.0.1:0",
        "--no-stun",
        "--wait-ms", str(max(15000, stream_ms + 15000)),
        "--audio-device", str(audio_device),
        "--sample-rate", str(sample_rate),
        "--log-stats", str(paths["csv_raw"]),
    ]
    args.extend(profile.args(stream_ms))
    if extra_args:
        args.extend(extra_args)
    return ManagedProcess(args, repo_root(), paths["stdout"], paths["stderr"], stdin_pipe=stdin_pipe).start(), paths


def start_connector(jam2, url, audio_device, sample_rate, profile, stream_ms, output_dir, extra_args=None, stdin_pipe=False):
    paths = side_paths(output_dir, "client")
    args = [
        jam2,
        "connect",
        url,
        "--wait-ms", str(max(15000, stream_ms + 15000)),
        "--audio-device", str(audio_device),
        "--sample-rate", str(sample_rate),
        "--log-stats", str(paths["csv_raw"]),
    ]
    args.extend(profile.args(stream_ms))
    if extra_args:
        args.extend(extra_args)
    return ManagedProcess(args, repo_root(), paths["stdout"], paths["stderr"], stdin_pipe=stdin_pipe).start(), paths


def collect_side_csv(paths):
    return copy_final_csv(paths["csv_raw"], paths["dir"])
