from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


SCENARIO_FORMAT = "jam2-debug-scenario"
DESCRIPTION_FORMAT = "jam2-debug-description"
AUTOMATION_FORMAT = "jam2-automation"
MAX_AUTOMATION_FRAME_BYTES = 64 * 1024
MAX_COLLECTED_AUTOMATION_EVENTS = 4096


def default_jam2(repo_root: Path) -> Path:
    name = "jam2.exe" if os.name == "nt" else "jam2"
    return repo_root / "release" / name


def run_logged(
    command: list[str], stdout_path: Path, stderr_path: Path, timeout: float = 60.0,
    env: dict[str, str] | None = None, close_fds: bool = True,
) -> dict[str, Any]:
    started = time.monotonic()
    with stdout_path.open("w", encoding="utf-8", newline="") as stdout, \
         stderr_path.open("w", encoding="utf-8", newline="") as stderr:
        try:
            process = subprocess.run(
                command, stdout=stdout, stderr=stderr, env=env, timeout=timeout,
                check=False, close_fds=close_fds,
            )
            code = process.returncode
            timed_out = False
        except subprocess.TimeoutExpired:
            code = -1
            timed_out = True
    return {
        "command": command,
        "return_code": code,
        "timed_out": timed_out,
        "duration_s": round(time.monotonic() - started, 3),
        "stdout": stdout_path.name,
        "stderr": stderr_path.name,
    }


class NativeCapabilities:
    def __init__(self, jam2: Path):
        self.jam2 = jam2.resolve()
        result = subprocess.run(
            [str(self.jam2), "debug", "describe", "--json"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=20, check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"debug describe failed ({result.returncode}): {result.stderr.strip()}")
        self.description = json.loads(result.stdout)
        if self.description.get("schema") != DESCRIPTION_FORMAT:
            raise RuntimeError("jam2 emitted an unsupported debug description format")
        if self.description.get("scenario_schema") != SCENARIO_FORMAT:
            raise RuntimeError("jam2 scenario format does not match the Python controller")
        if self.description.get("automation_protocol") != AUTOMATION_FORMAT:
            raise RuntimeError("jam2 automation format does not match the Python controller")
        self.profiles = {item["name"]: item for item in self.description["profiles"]}
        self.runtime_fields = {item["name"]: item for item in self.description["runtime_fields"]}

    def profile(self, name: str) -> dict[str, Any]:
        try:
            return dict(self.profiles[name])
        except KeyError as error:
            raise ValueError(f"unknown native profile: {name}") from error

    def validate_sparse_overrides(self, overrides: dict[str, Any]) -> None:
        unknown = sorted(set(overrides) - set(self.runtime_fields))
        if unknown:
            raise ValueError(f"unknown native overrides: {', '.join(unknown)}")
        for name, requested in overrides.items():
            field = self.runtime_fields[name]
            value = requested
            kind = field["type"]
            if kind == "boolean":
                if value in ("on", "enabled"):
                    value = True
                elif value in ("off", "disabled"):
                    value = False
                if not isinstance(value, bool):
                    raise ValueError(f"native override {name} must be a boolean")
                continue
            if kind == "integer":
                if not isinstance(value, int) or isinstance(value, bool):
                    raise ValueError(f"native override {name} must be an integer")
            elif kind == "number":
                if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
                    raise ValueError(f"native override {name} must be a finite number")
            elif kind == "string":
                if not isinstance(value, str) or not value:
                    raise ValueError(f"native override {name} must be a non-empty string")
                choices = field.get("choices", [])
                if choices and choices != ["channel-list"] and value not in choices:
                    raise ValueError(f"native override {name} is not a supported choice")
                continue
            else:
                raise ValueError(f"native field {name} has an unsupported described type")
            if value < field.get("minimum", value) or value > field.get("maximum", value):
                raise ValueError(f"native override {name} is outside its described bounds")


def write_scenario(path: Path, scenario: dict[str, Any]) -> None:
    if scenario.get("schema") != SCENARIO_FORMAT:
        raise ValueError("scenario must use the current unversioned format")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(scenario, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def native_manifest(root: Path) -> dict[str, Any]:
    path = root / "native-manifest.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "jam2-debug-manifest":
        raise RuntimeError(f"invalid native manifest: {path}")
    return data


@dataclass
class ReactiveProcess:
    process: subprocess.Popen[str]
    command_writer: Any
    event_reader: Any
    events: deque[dict[str, Any]]
    _reader_thread: threading.Thread
    _reader_state: dict[str, int]

    @classmethod
    def start(
        cls, jam2: Path, scenario_path: Path, stdout_path: Path, stderr_path: Path,
        event_callback: Callable[[dict[str, Any]], None] | None = None,
    ) -> "ReactiveProcess":
        command_read, command_write = os.pipe()
        event_read, event_write = os.pipe()
        os.set_inheritable(command_read, True)
        os.set_inheritable(event_write, True)
        os.set_inheritable(command_write, False)
        os.set_inheritable(event_read, False)
        if os.name == "nt":
            import msvcrt
            command_handle = msvcrt.get_osfhandle(command_read)
            event_handle = msvcrt.get_osfhandle(event_write)
        else:
            command_handle = command_read
            event_handle = event_write
        env = os.environ.copy()
        env["JAM2_AUTOMATION_COMMAND_HANDLE"] = str(command_handle)
        env["JAM2_AUTOMATION_EVENT_HANDLE"] = str(event_handle)
        stdout = stdout_path.open("w", encoding="utf-8", newline="")
        stderr = stderr_path.open("w", encoding="utf-8", newline="")
        process = subprocess.Popen(
            [str(jam2), "debug", "run", str(scenario_path)],
            stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
            text=True, env=env, close_fds=False,
        )
        os.close(command_read)
        os.close(event_write)
        command_writer = os.fdopen(command_write, "wb", buffering=0)
        event_reader = os.fdopen(event_read, "rb", buffering=0)
        events: deque[dict[str, Any]] = deque(maxlen=MAX_COLLECTED_AUTOMATION_EVENTS)
        reader_state = {"event_drops": 0}

        def read_exact(size: int) -> bytes:
            chunks = bytearray()
            while len(chunks) < size:
                chunk = event_reader.read(size - len(chunks))
                if not chunk:
                    return b""
                chunks.extend(chunk)
            return bytes(chunks)

        def read_events() -> None:
            try:
                while True:
                    prefix = read_exact(4)
                    if not prefix:
                        break
                    size = struct.unpack("<I", prefix)[0]
                    if not 0 < size <= MAX_AUTOMATION_FRAME_BYTES:
                        break
                    payload = read_exact(size)
                    if not payload:
                        break
                    event = json.loads(payload)
                    if len(events) == MAX_COLLECTED_AUTOMATION_EVENTS:
                        reader_state["event_drops"] += 1
                    events.append(event)
                    if event_callback:
                        event_callback(event)
            finally:
                stdout.close()
                stderr.close()

        reader = threading.Thread(target=read_events, name="jam2-automation-events", daemon=True)
        reader.start()
        return cls(process, command_writer, event_reader, events, reader, reader_state)

    def send(self, message: dict[str, Any]) -> None:
        payload = dict(message)
        payload["format"] = AUTOMATION_FORMAT
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if not 0 < len(encoded) <= MAX_AUTOMATION_FRAME_BYTES:
            raise ValueError("automation command exceeds native frame bounds")
        framed = memoryview(struct.pack("<I", len(encoded)) + encoded)
        while framed:
            written = self.command_writer.write(framed)
            if written is None or written <= 0:
                raise BrokenPipeError("automation command pipe stopped accepting data")
            framed = framed[written:]

    @property
    def event_drops(self) -> int:
        return self._reader_state["event_drops"]

    def wait(self, timeout: float) -> int:
        try:
            return self.process.wait(timeout=timeout)
        finally:
            self.command_writer.close()
            self._reader_thread.join(timeout=5)
            self.event_reader.close()
