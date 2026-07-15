from __future__ import annotations

import hashlib
import json
import platform
import socket
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass
class InvocationManifest:
    path: Path
    family: str
    invocation_id: str
    arguments: list[str]
    data: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.data = {
            "schema": "jam2-test-invocation",
            "family": self.family,
            "invocation_id": self.invocation_id,
            "started_utc": utc_now(),
            "state": "running",
            "arguments": self.arguments,
            "machine": {
                "id": socket.gethostname().lower(),
                "hostname": socket.gethostname(),
                "platform": platform.platform(),
                "python": sys.version,
            },
            "cases": [],
            "artifacts": [],
            "omissions": [],
        }
        self.write()

    def add_case(self, case: dict[str, Any]) -> None:
        self.data["cases"].append(case)
        self.write()

    def add_artifact(self, path: Path, root: Path, owner: str) -> None:
        if not path.is_file():
            return
        self.data["artifacts"].append({
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
            "owner": owner,
        })

    def refresh_artifacts(self) -> None:
        remaining = 1024 * 1024 * 1024
        files = []
        truncated = False
        for path in self.path.parent.rglob("*"):
            if not path.is_file() or path == self.path or path.name == self.path.name + ".tmp":
                continue
            if len(files) >= 4096:
                truncated = True
                break
            files.append(path)
        artifacts = []
        for path in sorted(files):
            size = path.stat().st_size
            item = {"path": path.relative_to(self.path.parent).as_posix(),
                    "bytes": size, "owner": "python-invocation"}
            if size <= remaining:
                item["sha256"] = sha256(path)
                remaining -= size
            else:
                item["hash_omitted"] = "invocation hash-byte bound reached"
            artifacts.append(item)
        self.data["artifacts"] = artifacts
        self.data["artifact_inventory"] = {
            "max_files": 4096, "max_hashed_bytes": 1024 * 1024 * 1024,
            "truncated_files": 1 if truncated else 0,
        }

    def finish(self, status: str, return_code: int, **fields: Any) -> None:
        self.refresh_artifacts()
        self.data.update(fields)
        self.data["state"] = status
        self.data["return_code"] = return_code
        self.data["finished_utc"] = utc_now()
        self.write()

    def write(self) -> None:
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(self.path)
