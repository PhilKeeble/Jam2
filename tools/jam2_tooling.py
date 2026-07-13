#!/usr/bin/env python3

import json
import os
import platform
import re
import shutil
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path


_JAM_URL_KEY = re.compile(r"(?i)([?&]key=)[0-9a-f]+")
_SESSION_KEY_ASSIGNMENT = re.compile(r"(?i)(--session-key=)[0-9a-f]+")
_SESSION_KEY_ARGUMENT = re.compile(r"(?i)(--session-key\s+)[0-9a-f]+")


def redact_text(value):
    text = str(value)
    text = _JAM_URL_KEY.sub(r"\1<redacted>", text)
    text = _SESSION_KEY_ASSIGNMENT.sub(r"\1<redacted>", text)
    return _SESSION_KEY_ARGUMENT.sub(r"\1<redacted>", text)


def redact_cli_args(args):
    redacted = []
    hide_next = False
    for item in args:
        text = str(item)
        if hide_next:
            redacted.append("<redacted>")
            hide_next = False
            continue
        redacted.append(redact_text(text))
        hide_next = text == "--session-key"
    return redacted


def redact_structure(value):
    if isinstance(value, dict):
        return {str(key): redact_structure(item) for key, item in value.items()}
    if isinstance(value, list):
        return [redact_structure(item) for item in value]
    if isinstance(value, tuple):
        return [redact_structure(item) for item in value]
    if isinstance(value, str):
        return redact_text(value)
    return value


def new_run_manifest(tool, argv, **settings):
    return {
        "schema_version": 1,
        "run_id": uuid.uuid4().hex,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "tool": str(tool),
        "argv": redact_cli_args(argv),
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
        "settings": settings,
    }


def repo_root():
    return Path(__file__).resolve().parents[1]


def default_jam2_path():
    root = repo_root()
    name = "jam2.exe" if os.name == "nt" else "jam2"
    return root / "release" / name


def ensure_dir(path):
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2)
            handle.write("\n")
        for attempt in range(20):
            try:
                tmp.replace(path)
                return
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.05)
    finally:
        if tmp.exists():
            try:
                tmp.unlink()
            except OSError:
                pass


def safe_test_id(value):
    out = []
    for char in value:
        if char.isalnum() or char in ("-", "_", "."):
            out.append(char)
        else:
            out.append("_")
    return "".join(out)


def run_dir(base, test_id, side, run_index):
    return ensure_dir(base / safe_test_id(test_id) / side / f"run_{run_index:02d}")


def newest_csv(log_dir):
    files = sorted(log_dir.glob("jam2_stats_*.csv"), key=lambda path: path.stat().st_mtime)
    return files[-1] if files else None


def copy_final_csv(log_dir, destination):
    csv = newest_csv(log_dir)
    if csv is None:
        return None
    target = destination / "stats.csv"
    shutil.copy2(csv, target)
    return target


def print_flush(message):
    print(message, flush=True)


def wait_for_file(path, timeout_s):
    deadline = time.monotonic() + timeout_s if timeout_s > 0 else None
    while True:
        if path.exists():
            return True
        if deadline is not None and time.monotonic() >= deadline:
            return False
        time.sleep(0.25)


def fail(message):
    print(message, file=sys.stderr, flush=True)
    return 1
