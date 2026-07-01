#!/usr/bin/env python3

import json
import os
import shutil
import sys
import time
from pathlib import Path


def repo_root():
    return Path(__file__).resolve().parents[1]


def default_jam2_path():
    root = repo_root()
    name = "jam2.exe" if os.name == "nt" else "jam2"
    return root / "build" / name


def ensure_dir(path):
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_json(path, payload):
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")
    tmp.replace(path)


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
