from __future__ import annotations

from pathlib import Path
from typing import Any

from .artifacts import allocate_invocation
from .manifest import InvocationManifest


def run(args: Any, repo: Path, arguments: list[str]) -> int:
    if args.family == "benchmark":
        from .benchmark import run as run_benchmark
        return run_benchmark(args, repo, arguments)

    run_id = (getattr(args, "selection", None) or
              getattr(args, "connectivity_mode", None) or "run")
    invocation = allocate_invocation(
        args.family, repo / "tools", getattr(args, "output", None),
        run_id, getattr(args, "clean", False),
    )
    manifest = InvocationManifest(
        invocation.root / "invocation-manifest.json",
        args.family, invocation.invocation_id, arguments,
    )
    print(f"[{args.family}] artifacts: {invocation.root}", flush=True)

    if args.family == "validate":
        from .validation import run as run_validation
        return run_validation(
            args.selection, repo, args.jam2, invocation, manifest,
            args.real_device,
        )
    if args.family == "stress":
        from .stress import run as run_stress
        return run_stress(args, repo, invocation, manifest)

    from .connectivity import run as run_connectivity
    return run_connectivity(args, invocation, manifest)
