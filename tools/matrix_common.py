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


def load_matrix(path):
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    tests = data.get("tests")
    if not isinstance(tests, list) or not tests:
        raise ValueError("matrix file must contain a non-empty tests array")
    seen = set()
    for test in tests:
        test_id = test.get("id")
        if not isinstance(test_id, str) or not test_id:
            raise ValueError("each test requires a non-empty string id")
        if test_id in seen:
            raise ValueError(f"duplicate test id: {test_id}")
        seen.add(test_id)
    return data


def list_arg(value, name):
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"{name} must be an array of strings")
    return value


def test_args(matrix, test, side):
    defaults = matrix.get("defaults", {})
    stream_ms = int(defaults.get("stream_ms", 120000))
    stream_linger_ms = int(defaults.get("stream_linger_ms", 500))
    args = []
    args.extend(list_arg(defaults.get("common_args", []), "defaults.common_args"))
    args.extend(list_arg(defaults.get(f"{side}_args", []), f"defaults.{side}_args"))
    args.extend(list_arg(test.get("common_args", []), f"{test['id']}.common_args"))
    args.extend(list_arg(test.get(f"{side}_args", []), f"{test['id']}.{side}_args"))
    args.extend(["--stream-ms", str(stream_ms), "--stream-linger-ms", str(stream_linger_ms)])
    return args


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
