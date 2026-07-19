from __future__ import annotations

import os
import re
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
    "fuzz": "fuzz_logs",
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
    """Return a readable bounded component, hashing only unusually long ids."""
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip()).strip(".-") or "run"
    if len(safe) <= 64:
        return safe
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:8]
    return f"{safe[:55]}-{digest}"


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
    direct_output = output_parent is not None
    parent = _validated_parent(output_parent or tools_dir)
    family_root = parent if direct_output else parent / FAMILY_DIRS[family]
    expected = parent if direct_output else parent / FAMILY_DIRS[family]
    resolved_family = _resolved_without_requirement(family_root)
    resolved_expected = _resolved_without_requirement(expected)
    repository = _resolved_without_requirement(tools_dir.parent)
    protected = {repository, _resolved_without_requirement(tools_dir)}
    if resolved_family != resolved_expected or resolved_family in protected:
        raise ValueError("refusing unsafe command-family artifact root")
    if not direct_output:
        try:
            resolved_family.relative_to(parent)
        except ValueError as error:
            raise ValueError("command-family artifact root escapes its selected parent") from error
    if family_root.exists() and family_root.is_symlink():
        raise ValueError("command-family artifact root must not be a symlink")
    if clean and family_root.exists():
        shutil.rmtree(family_root)
    family_root.mkdir(parents=True, exist_ok=True)
    # Minute precision keeps paths short and readable. A small numeric suffix
    # handles the rare case where another invocation starts in the same minute.
    base = datetime.now(timezone.utc).strftime("%Y%m%dT%H%MZ")
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
    direct_output = output_parent is not None
    parent = _validated_parent(output_parent or tools_dir)
    family_root = parent if direct_output else parent / FAMILY_DIRS[family]
    expected = parent if direct_output else parent / FAMILY_DIRS[family]
    if _resolved_without_requirement(family_root) != _resolved_without_requirement(expected):
        raise ValueError("command-family artifact root escapes its selected parent")
    repository = _resolved_without_requirement(tools_dir.parent)
    if _resolved_without_requirement(family_root) in {
            repository, _resolved_without_requirement(tools_dir)}:
        raise ValueError("refusing unsafe command-family artifact root")
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
    role: str,
    case_id: str,
    run_id: str,
    attempt_id: str,
    *,
    create: bool = True,
) -> Path:
    if invocation.family != "benchmark":
        raise ValueError("benchmark layout requires a benchmark invocation")
    if role not in {"coordinator", "agent"}:
        raise ValueError("benchmark artifact role must be coordinator or agent")
    if not suite_id:
        raise ValueError("benchmark suite identity is required")
    case = normalized_path_id(case_id)
    run_text = run_id.removeprefix("run-")
    run = str(int(run_text)) if run_text.isdigit() else normalized_path_id(run_text)
    # Coordinator and agent use the same relative execution root on their
    # respective hosts. The coordinator prefixes uploaded agent filenames when
    # merging them into its copy of this directory.
    path = invocation.root / case / run
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
