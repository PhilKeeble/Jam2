#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Combine Jam2 matrix stats.csv files into one CSV.")
    parser.add_argument("--logs", default=str(Path(__file__).with_name("logs")))
    parser.add_argument("--output", default="")
    parser.add_argument("--side", choices=("server", "client", "all"), default="all")
    return parser.parse_args()


def discover_stats(logs_dir, side):
    pattern = "*/"
    paths = []
    for test_dir in sorted(path for path in logs_dir.iterdir() if path.is_dir() and path.name != "public"):
        sides = ["server", "client"] if side == "all" else [side]
        for side_name in sides:
            side_dir = test_dir / side_name
            if not side_dir.exists():
                continue
            for run_dir in sorted(path for path in side_dir.iterdir() if path.is_dir()):
                stats = run_dir / "stats.csv"
                if stats.exists():
                    paths.append((test_dir.name, side_name, run_dir.name, stats))
    return paths


def run_number(run_name):
    prefix = "run_"
    if run_name.startswith(prefix):
        try:
            return int(run_name[len(prefix):])
        except ValueError:
            return 0
    return 0


def read_json(path):
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def run_metadata(logs_dir, test_id, run_name):
    server_meta = read_json(logs_dir / test_id / "server" / run_name / "metadata.json")
    client_meta = read_json(logs_dir / test_id / "client" / run_name / "upload.json")
    server_network = server_meta.get("network_type", "unknown")
    client_network = client_meta.get("network_type", "unknown")
    network_profile = server_meta.get("network_profile", "unknown")
    if server_network == "wifi" or client_network == "wifi":
        combined = "wifi"
    elif server_network == "wired" and client_network == "wired":
        combined = "wired"
    else:
        combined = "unknown"
    return {
        "matrix_network_type": combined,
        "matrix_server_network_type": server_network,
        "matrix_client_network_type": client_network,
        "matrix_network_profile": network_profile,
    }


def default_output_path(logs_dir, side):
    name = "combined_stats.csv" if side == "all" else f"combined_stats_{side}.csv"
    return logs_dir / name


def collect_matrix_csv(logs_dir, output=None, side="all"):
    logs_dir = Path(logs_dir)
    if not logs_dir.exists():
        raise SystemExit(f"logs directory not found: {logs_dir}")
    output = Path(output) if output else default_output_path(logs_dir, side)
    rows = discover_stats(logs_dir, side)
    if not rows:
        raise SystemExit(f"no stats.csv files found under {logs_dir}")

    loaded_rows = []
    header = [
        "matrix_test_id",
        "matrix_side",
        "matrix_run",
        "matrix_stats_path",
        "matrix_network_type",
        "matrix_server_network_type",
        "matrix_client_network_type",
        "matrix_network_profile",
    ]
    seen_fields = set(header)
    for test_id, side_name, run_name, stats_path in rows:
        with open(stats_path, "r", encoding="utf-8", newline="") as in_file:
            reader = csv.DictReader(in_file)
            if reader.fieldnames is None:
                continue
            for field in reader.fieldnames:
                if field not in seen_fields:
                    header.append(field)
                    seen_fields.add(field)
            for row in reader:
                out_row = {
                    "matrix_test_id": test_id,
                    "matrix_side": side_name,
                    "matrix_run": run_number(run_name),
                    "matrix_stats_path": str(stats_path),
                }
                out_row.update(run_metadata(logs_dir, test_id, run_name))
                out_row.update(row)
                loaded_rows.append(out_row)

    written = 0
    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as out_file:
        writer = csv.DictWriter(out_file, fieldnames=header, extrasaction="ignore")
        writer.writeheader()
        for row in loaded_rows:
            writer.writerow(row)
            written += 1

    return output, written, len(rows)


def main():
    args = parse_args()
    output, written, file_count = collect_matrix_csv(args.logs, args.output or None, args.side)
    print(f"wrote {written} row(s) from {file_count} file(s) to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
