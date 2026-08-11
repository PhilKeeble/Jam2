#!/usr/bin/env python3
"""Build one immutable JamTaster release component on its target platform."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile


COMPONENT_VERSION = "1.0.0"
PROTOCOL_VERSION = 1


def package_platform(accelerator: str) -> str:
    system = platform.system().lower()
    system = "windows" if system == "windows" else "macos" if system == "darwin" else system
    machine = platform.machine().lower()
    if machine == "amd64":
        machine = "x86_64"
    value = f"{system}-{machine}"
    if system == "windows":
        value += f"-{accelerator}"
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_tree(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--release-base-url",
        default=f"https://github.com/PhilKeeble/Jam2/releases/download/jamtaster-{COMPONENT_VERSION}",
    )
    args = parser.parse_args()

    source_root = Path(__file__).resolve().parents[1] / "worker"
    component_root = args.component_root.expanduser().resolve()
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    python = component_root / "runtime" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.is_file():
        raise SystemExit(f"private component Python is missing: {python}")
    installed_manifest = json.loads(
        (component_root / "component.json").read_text(encoding="utf-8")
    )
    accelerator = str(installed_manifest.get("accelerator", ""))
    expected = {"apple"} if platform.system() == "Darwin" else {"cpu", "cuda"}
    if accelerator not in expected:
        raise SystemExit(
            f"component accelerator {accelerator!r} is invalid for {platform.system()}"
        )
    subprocess.run(
        [str(python), str(source_root / "JamTaster.py"),
         "--component-root", str(component_root), "doctor", "--json"],
        check=True,
    )
    subprocess.run([str(python), "-m", "pip", "install", "PyInstaller==6.15.0"], check=True)

    work = output / "work"
    dist = output / "dist"
    for path in (work, dist):
        if path.exists():
            shutil.rmtree(path)
    collect = [
        "torch", "torchaudio", "demucs_infer", "beat_this", "basic_pitch",
        "adtof_pytorch", "librosa", "python_stretch",
    ]
    command = [
        str(python), "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir",
        "--name", "JamTasterWorker", "--distpath", str(dist),
        "--workpath", str(work), "--specpath", str(work),
    ]
    for package in collect:
        command.extend(["--collect-all", package])
    command.append(str(source_root / "JamTaster.py"))
    subprocess.run(command, check=True, cwd=source_root)

    package_id = package_platform(accelerator)
    name = f"jamtaster-{COMPONENT_VERSION}-{package_id}"
    package = output / name
    if package.exists():
        shutil.rmtree(package)
    package.mkdir()
    worker_destination = package / "worker"
    shutil.copytree(dist / "JamTasterWorker", worker_destination)
    copy_tree(component_root / "models", package / "models")
    # Only inference material belongs in a release. Pip archives, source
    # downloads, benchmark caches and compiled Numba scratch data are local
    # installation debris and can add several unnecessary gigabytes.
    copy_tree(component_root / "cache" / "torch", package / "cache" / "torch")
    worker_name = "JamTasterWorker.exe" if os.name == "nt" else "JamTasterWorker"
    manifest = {
        "format": "jamtaster-component-v1",
        "version": COMPONENT_VERSION,
        "protocol": PROTOCOL_VERSION,
        "mode": "packaged",
        "accelerator": accelerator,
        "worker": f"worker/{worker_name}",
        "models_root": "models",
    }
    (package / "component.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    archive = output / f"{name}.tar.gz"
    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(package, arcname=name)
    release = {
        "format": "jamtaster-release-v1",
        "version": COMPONENT_VERSION,
        "protocol": PROTOCOL_VERSION,
        "platform": package_id,
        "accelerator": accelerator,
        "bytes": archive.stat().st_size,
        "sha256": sha256(archive),
        "url": f"{args.release_base_url.rstrip('/')}/{archive.name}",
    }
    (output / f"{name}.json").write_text(
        json.dumps(release, indent=2) + "\n", encoding="utf-8"
    )
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
