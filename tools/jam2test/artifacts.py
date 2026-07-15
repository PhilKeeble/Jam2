from __future__ import annotations

import os
import re
import secrets
import shutil
import hashlib
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


FAMILY_DIRS = {
    "validate": "validate_logs",
    "stress": "stress_logs",
    "benchmark": "benchmark_logs",
    "connectivity": "connectivity_logs",
}

# Native CSV/recording writers still pass through Windows APIs with the
# legacy 260-character file-path ceiling. Keep enough room beneath an attempt
# root for generated filenames and fail before a live suite starts.
MAX_WINDOWS_NATIVE_ATTEMPT_ROOT_CHARS = 200
MAX_NATIVE_ATTEMPT_ROOT_CHARS = 2048
MAX_OUTPUT_PARENT_CHARS = 2048


def _safe_id(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip()).strip(".-")
    return (cleaned or "run")[:64]


def normalized_path_id(value: str) -> str:
    """Return a stable, collision-resistant short component for nested trees."""
    safe = _safe_id(value)
    if len(safe) <= 24:
        return safe
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:8]
    return f"{safe[:15]}-{digest}"


def _resolved_without_requirement(path: Path) -> Path:
    return Path(os.path.realpath(path))


def _validated_parent(path: Path) -> Path:
    resolved = _resolved_without_requirement(path)
    if len(str(resolved)) > MAX_OUTPUT_PARENT_CHARS:
        raise ValueError("artifact output parent exceeds its 2048-character bound")
    return resolved


@dataclass(frozen=True)
class InvocationArtifacts:
    family: str
    parent: Path
    family_root: Path
    invocation_id: str
    root: Path

    def path(self, *parts: str) -> Path:
        target = self.root.joinpath(*parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        return target


def allocate_invocation(
    family: str,
    tools_dir: Path,
    output_parent: Path | None = None,
    run_id: str = "run",
    clean: bool = False,
) -> InvocationArtifacts:
    if family not in FAMILY_DIRS:
        raise ValueError(f"unknown artifact family: {family}")
    parent = _validated_parent(output_parent or tools_dir)
    family_root = parent / FAMILY_DIRS[family]
    expected = parent / FAMILY_DIRS[family]
    resolved_family = _resolved_without_requirement(family_root)
    resolved_expected = _resolved_without_requirement(expected)
    repository = _resolved_without_requirement(tools_dir.parent)
    if resolved_family != resolved_expected or resolved_family in (parent, repository):
        raise ValueError("refusing unsafe command-family artifact root")
    try:
        resolved_family.relative_to(parent)
    except ValueError as error:
        raise ValueError("command-family artifact root escapes its selected parent") from error
    if family_root.exists() and family_root.is_symlink():
        raise ValueError("command-family artifact root must not be a symlink")
    if clean and family_root.exists():
        shutil.rmtree(family_root)
    family_root.mkdir(parents=True, exist_ok=True)
    # Keep the documented nested benchmark layout usable on Windows without
    # requiring a machine-wide long-path policy. Random suffixes still make
    # same-second invocations collision-safe.
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = f"{timestamp}_{secrets.token_hex(4)}"
    for collision in range(100):
        invocation_id = base if collision == 0 else f"{base}_{collision}"
        root = family_root / invocation_id
        try:
            root.mkdir()
            return InvocationArtifacts(family, parent, family_root, invocation_id, root)
        except FileExistsError:
            continue
    raise RuntimeError("could not allocate a unique artifact invocation directory")


def adopt_invocation(
    family: str, tools_dir: Path, invocation_id: str,
    output_parent: Path | None = None, clean: bool = False,
) -> InvocationArtifacts:
    if family not in FAMILY_DIRS or _safe_id(invocation_id) != invocation_id:
        raise ValueError("invalid coordinator-issued invocation identity")
    parent = _validated_parent(output_parent or tools_dir)
    family_root = parent / FAMILY_DIRS[family]
    if _resolved_without_requirement(family_root) != _resolved_without_requirement(parent / FAMILY_DIRS[family]):
        raise ValueError("command-family artifact root escapes its selected parent")
    if family_root.exists() and family_root.is_symlink():
        raise ValueError("command-family artifact root must not be a symlink")
    if clean and family_root.exists(): shutil.rmtree(family_root)
    family_root.mkdir(parents=True, exist_ok=True)
    root = family_root / invocation_id
    root.mkdir(exist_ok=False)
    return InvocationArtifacts(family, parent, family_root, invocation_id, root)


def benchmark_attempt_path(
    invocation: InvocationArtifacts,
    suite_id: str,
    machine_id: str,
    case_id: str,
    run_id: str,
    attempt_id: str,
    *,
    create: bool = True,
) -> Path:
    if invocation.family != "benchmark":
        raise ValueError("benchmark layout requires a benchmark invocation")
    values = [normalized_path_id(value) for value in (suite_id, machine_id, case_id, run_id, attempt_id)]
    path = invocation.root / "suites" / values[0] / "machines" / values[1]
    path = path / "cases" / values[2] / "runs" / values[3] / "attempts" / values[4]
    if create:
        path.mkdir(parents=True, exist_ok=False)
    return path


def validate_native_attempt_root(path: Path) -> None:
    length = len(str(_resolved_without_requirement(path)))
    if length > MAX_NATIVE_ATTEMPT_ROOT_CHARS:
        raise ValueError(
            "benchmark native attempt path exceeds its 2048-character bound; "
            "choose a shorter --output root"
        )
    if os.name == "nt" and length > MAX_WINDOWS_NATIVE_ATTEMPT_ROOT_CHARS:
        raise ValueError(
            "benchmark native attempt path exceeds the Windows path budget; "
            "choose a shorter --output root"
        )
