#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from jam2test.native import default_jam2


def parser() -> argparse.ArgumentParser:
    root = Path(__file__).resolve().parents[1]
    artifact_output_help = (
        "artifact root; the timestamped invocation is created directly beneath it")
    result = argparse.ArgumentParser(
        description="Jam2 benchmark, connectivity, fuzz, and offline analysis tooling")
    families = result.add_subparsers(dest="family", required=True)

    benchmark = families.add_parser("benchmark", help="two-host coordinator/agent workflow and analysis")
    benchmark_modes = benchmark.add_subparsers(dest="benchmark_mode", required=True)
    for mode in ("coordinator", "agent"):
        item = benchmark_modes.add_parser(mode)
        item.add_argument("suite", choices=("core", "full"), nargs="?", default="core")
        item.add_argument("--jam2", type=Path, default=default_jam2(root))
        item.add_argument("--output", type=Path, help=artifact_output_help)
        item.add_argument("--clean", action="store_true")
        item.add_argument("--machine-id", required=True)
        item.add_argument("--sample-rate", type=int, default=48000)
        item.add_argument("--control", default="0.0.0.0:49000" if mode == "coordinator" else "")
        audio = item.add_mutually_exclusive_group(required=True)
        audio.add_argument("--audio-device", type=int)
        audio.add_argument("--headless-audio", action="store_true")
        item.add_argument("--network-profile", choices=("auto", "wired", "wifi", "unknown"),
                          default="auto", help="recorded machine/network label; never changes tuning")
        item.add_argument("--case-timeout-s", type=float, default=0.0)
        if mode == "agent":
            item.add_argument("--coordinator", required=True)
            item.add_argument("--delete-after-upload", action="store_true")
            item.add_argument("--connect-timeout-s", type=float, default=120.0)
        else:
            item.add_argument("--audio-bind", default="0.0.0.0:49001")
            item.add_argument("--public-audio-host")
            item.add_argument("--initial-agent-timeout-s", type=float, default=120.0)
            item.add_argument("--upload-timeout-s", type=float, default=120.0)
            item.add_argument("--case-retry-limit", type=int, default=1)
            item.add_argument("--finish-grace-s", type=float, default=30.0)
    listing = benchmark_modes.add_parser("list", help="list the fixed versioned case catalog")
    listing.add_argument("suite", choices=("core", "full"), nargs="?", default="core")
    listing.add_argument("--jam2", type=Path, default=default_jam2(root))
    analyze = benchmark_modes.add_parser("analyze")
    analyze.add_argument("results", type=Path)
    analyze.add_argument("--output", type=Path, help=artifact_output_help)
    analyze.add_argument("--clean", action="store_true")
    package = benchmark_modes.add_parser("package", help="create a compact local submission archive")
    package.add_argument("results", type=Path)
    package.add_argument("--output", type=Path, help="exact output .zip path")

    connectivity = families.add_parser("connectivity", help="independent STUN and direct UDP diagnostics")
    connectivity_modes = connectivity.add_subparsers(dest="connectivity_mode", required=True)
    stun = connectivity_modes.add_parser("stun")
    stun.add_argument("--server", action="append", default=["stun.l.google.com:19302"])
    stun.add_argument("--bind", default="0.0.0.0:0")
    stun.add_argument("--timeout-s", type=float, default=3.0)
    direct = connectivity_modes.add_parser("direct")
    direct.add_argument("--bind", default="0.0.0.0:49001")
    direct.add_argument("--peer-token")
    direct.add_argument("--direct-host", help="host/IP advertised in the share token")
    direct.add_argument("--name", default="", help="machine label included in the share token")
    direct.add_argument("--duration-s", type=float, default=10.0)
    direct.add_argument("--interval-s", type=float, default=0.5)
    for item in (stun, direct):
        item.add_argument("--output", type=Path, help=artifact_output_help)
        item.add_argument("--clean", action="store_true")

    fuzz = families.add_parser("fuzz", help="run bounded native parser mutation and corpus checks")
    fuzz.add_argument("selection", choices=("all", "control", "udp", "asset", "wav"), nargs="?", default="all")
    fuzz.add_argument("--jam2", type=Path, default=default_jam2(root))
    fuzz.add_argument("--output", type=Path, help=artifact_output_help)
    fuzz.add_argument("--clean", action="store_true")
    fuzz.add_argument("--seed", type=int, default=1)
    fuzz.add_argument("--iterations", type=int, default=32,
                      help="bounded inputs per native target (1..10000)")
    fuzz.add_argument("--input-timeout-s", type=float, default=2.0)
    fuzz.add_argument("--total-timeout-s", type=float, default=180.0)
    fuzz.add_argument("--max-input-bytes", type=int, default=64 * 1024)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repo = Path(__file__).resolve().parents[1]
    from jam2test.dispatch import run
    return run(args, repo, sys.argv[1:] if argv is None else argv)


if __name__ == "__main__":
    raise SystemExit(main())
