from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import venv

from . import __version__
from .component import component_health, runtime_variant, write_component_manifest
from .events import json_line_event
from .models import configure_local_caches, fetch_models, resolve_device
from .paths import (
    CACHE_ROOT, COMPONENT_ROOT, MANIFEST_PATH, SOURCE_ROOT, VENV_ROOT, venv_python,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="JamTaster.py",
        description="Jam2's isolated WAV analysis component.",
    )
    parser.add_argument("--component-root", help="override the versioned JamTaster installation root")
    parser.add_argument("--version", action="version", version=f"JamTaster {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)

    install = commands.add_parser("install", help="create a development component installation")
    install.add_argument("--cpu", action="store_true", help="install CPU-only PyTorch")

    repair = commands.add_parser("repair", help="repair only unhealthy installed component parts")
    repair.add_argument("--cpu", action="store_true", help="repair with CPU-only PyTorch")

    models = commands.add_parser("models", help="manage pinned local model weights")
    model_commands = models.add_subparsers(dest="models_command", required=True)
    fetch = model_commands.add_parser("fetch", help="fetch all pinned model sources and weights")
    fetch.add_argument("--device", default="auto", choices=("auto", "cpu", "cuda"))

    doctor = commands.add_parser("doctor", help="report dependency, model and accelerator status")
    doctor.add_argument("--json", action="store_true", help="write one machine-readable result")
    doctor.add_argument("--progress", action="store_true", help=argparse.SUPPRESS)

    worker = commands.add_parser("worker", help="execute one versioned Jam2 request")
    worker.add_argument("--request", required=True, help="path to a JSON request")

    taste = commands.add_parser("taste", help="fully analyse one WAV and prepare a JamJar")
    taste.add_argument("input", help="mono PCM16 Jam2 loopback WAV")
    taste.add_argument("--project-root", required=True, help="owning Jam2 song/workspace folder")
    taste.add_argument("--name", required=True, help="Jam2 song name")
    _add_analysis_options(taste)
    return parser


def _add_analysis_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", default="auto", choices=("auto", "cpu", "cuda"))
    parser.add_argument("--sections", default="auto", choices=("auto", "single"))
    parser.add_argument("--bpm", type=float)
    parser.add_argument("--meter", default="auto", choices=("auto", *[str(value) for value in range(2, 13)]))
    parser.add_argument("--no-arrangement-loop", action="store_true")
    parser.add_argument("--no-time-stretch", action="store_true")
    parser.add_argument("--demucs-model", default="htdemucs_ft")
    parser.add_argument("--beat-model", default="final0")
    parser.add_argument("--drum-thresholds", default="0.12,0.16,0.12,0.05,0.20")
    parser.add_argument("--drum-candidate-scale", type=float, default=0.30)
    parser.add_argument("--drum-division", type=int, default=4, choices=(2, 3, 4, 6, 8))
    parser.add_argument("--drum-ghost-energy-ratio", type=float, default=0.50)
    parser.add_argument("--drum-accent-energy-ratio", type=float, default=0.85)
    parser.add_argument("--drum-repair-min-repeats", type=int, default=3)
    parser.add_argument("--drum-repair-neighborhood-bars", type=int, default=8)
    parser.add_argument("--no-drum-repair", action="store_true")
    parser.add_argument("--bass-min-midi", type=int, default=28)
    parser.add_argument("--bass-max-midi", type=int, default=60)
    parser.add_argument("--drift-warning-ms", type=float, default=100.0)
    parser.add_argument("--local-drift-warning-ms", type=float, default=45.0)
    parser.add_argument("--local-warp-threshold-ms", type=float, default=45.0)
    parser.add_argument("--force", action="store_true", help="ignore reusable saved analysis")


def _inside_runtime() -> bool:
    if bool(getattr(sys, "frozen", False)):
        return True
    try:
        return Path(sys.prefix).resolve() == VENV_ROOT.resolve()
    except OSError:
        return False


def _reexec_in_runtime(argv: list[str]) -> int:
    python = venv_python()
    if not python.is_file():
        raise RuntimeError("JamTaster is not installed")
    environment = dict(os.environ)
    environment["JAMTASTER_COMPONENT_ROOT"] = str(COMPONENT_ROOT)
    completed = subprocess.run(
        [str(python), str(SOURCE_ROOT / "JamTaster.py"), *argv],
        env=environment,
        check=False,
    )
    return completed.returncode


_PIP_RAW_PROGRESS = re.compile(r"^Progress\s+(\d+)\s+(?:of|/)\s+(\d+)", re.IGNORECASE)


def _run(
    command: list[str],
    *,
    environment: dict[str, str] | None = None,
    activity: str = "",
    progress_range: tuple[int, int] | None = None,
) -> None:
    """Run an installer child while preserving logs and translating pip's raw
    byte counts into the stable progress protocol consumed by Jam2."""
    process = subprocess.Popen(
        command,
        cwd=SOURCE_ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    transfer_started = time.monotonic()
    last_reported = transfer_started
    previous_bytes = 0
    assert process.stdout is not None
    for output in process.stdout:
        line = output.rstrip("\r\n")
        match = _PIP_RAW_PROGRESS.match(line.strip())
        if not match:
            print(line, flush=True)
            continue
        current = int(match.group(1))
        total = max(1, int(match.group(2)))
        now = time.monotonic()
        if current < previous_bytes:
            transfer_started = now
        previous_bytes = current
        if now - last_reported < 0.5 and current < total:
            continue
        seconds = max(0.001, now - transfer_started)
        current_mb = current / (1024.0 * 1024.0)
        total_mb = total / (1024.0 * 1024.0)
        speed = current_mb / seconds
        percent = progress_range[0] if progress_range else 0
        if progress_range:
            percent += round((progress_range[1] - progress_range[0]) * current / total)
        _install_progress(
            f"{activity}: {current_mb:.1f} / {total_mb:.1f} MB at {speed:.1f} MB/s",
            percent,
        )
        last_reported = now
    return_code = process.wait()
    if return_code != 0:
        raise subprocess.CalledProcessError(return_code, command)


def _install_progress(message: str, percent: int) -> None:
    print(json.dumps({
        "format": "jamtaster-install-progress-v1",
        "message": message,
        "percent": percent,
    }, separators=(",", ":")), flush=True)


def _torch_install_command(
    python: str,
    cpu: bool,
    *,
    os_name: str | None = None,
    platform_name: str | None = None,
) -> list[str]:
    """Build the pinned platform runtime command without leaking CUDA onto macOS."""
    os_name = os.name if os_name is None else os_name
    platform_name = sys.platform if platform_name is None else platform_name
    command = [python, "-m", "pip", "install", "--progress-bar", "raw"]
    if os_name == "nt":
        command.extend([
            "--index-url",
            "https://download.pytorch.org/whl/cpu"
            if cpu else "https://download.pytorch.org/whl/cu126",
        ])
    elif cpu and platform_name != "darwin":
        command.extend([
            "--index-url", "https://download.pytorch.org/whl/cpu",
        ])
    command.extend(["torch==2.9.1", "torchaudio==2.9.1"])
    return command


def _pip_bootstrap_command(python: str) -> list[str]:
    """Use syntax supported by the old pip bundled with a fresh Python 3.10 venv."""
    return [
        python, "-m", "pip", "install", "--disable-pip-version-check",
        "--progress-bar", "off", "--upgrade", "pip==25.1.1",
        "setuptools==80.9.0", "wheel==0.45.1",
    ]


def install_component(cpu: bool) -> None:
    configure_local_caches()
    _install_progress("Preparing JamTaster's private application runtime", 2)
    if sys.version_info[:2] != (3, 10):
        raise RuntimeError(
            f"JamTaster requires Python 3.10 for its development installer; found "
            f"{sys.version_info.major}.{sys.version_info.minor}"
        )
    if VENV_ROOT.exists() and not venv_python().is_file():
        raise RuntimeError(f"{VENV_ROOT} exists but is not a usable component runtime")
    COMPONENT_ROOT.mkdir(parents=True, exist_ok=True)
    if not venv_python().is_file():
        _install_progress("Creating JamTaster's private Python runtime", 5)
        venv.EnvBuilder(with_pip=True).create(VENV_ROOT)
    python = str(venv_python())
    environment = dict(os.environ)
    environment["JAMTASTER_COMPONENT_ROOT"] = str(COMPONENT_ROOT)
    _install_progress("Preparing JamTaster's private package installer", 10)
    _run(_pip_bootstrap_command(python), environment=environment)
    _install_progress(
        "Installing the selected private PyTorch runtime. This is a large download "
        "and can take several minutes; you can safely close this dialog and return later.",
        18,
    )
    _run(
        _torch_install_command(python, cpu),
        environment=environment,
        activity="Downloading the private PyTorch runtime",
        progress_range=(18, 39),
    )
    _install_progress(
        "Installing JamTaster's private analysis libraries. This can take a while; "
        "you can safely close this dialog and return later.",
        40,
    )
    _run([
        python, "-m", "pip", "install", "--disable-pip-version-check",
        "--progress-bar", "raw", "-r", str(SOURCE_ROOT / "requirements.txt"),
    ], environment=environment, activity="Downloading analysis libraries", progress_range=(40, 59))
    _install_progress(
        "Installing pinned JamTaster models. Model downloads may take several minutes; "
        "you can safely close this dialog and return later.",
        60,
    )
    _run([
        python, str(SOURCE_ROOT / "JamTaster.py"), "--component-root", str(COMPONENT_ROOT),
        "models", "fetch", "--device", "cpu" if cpu else "auto",
    ], environment=environment)
    _install_progress("Checking the completed JamTaster installation", 97)
    _run([
        python, str(SOURCE_ROOT / "JamTaster.py"), "--component-root", str(COMPONENT_ROOT),
        "doctor", "--json",
    ], environment=environment)
    # Pip's CUDA wheel cache alone can duplicate several gigabytes of the
    # installed runtime. Release packages never include installer caches, and
    # a development repair can download them again, so discard only these
    # component-owned installation artifacts after successful validation.
    for disposable in (CACHE_ROOT / "pip", CACHE_ROOT / "downloads"):
        if disposable.is_dir():
            shutil.rmtree(disposable)
    _install_progress("JamTaster installation complete", 100)


def repair_component(cpu: bool) -> None:
    """Repair an existing private runtime without recreating or reinstalling it."""
    configure_local_caches()
    if not venv_python().is_file():
        raise RuntimeError("JamTaster's private Python runtime is missing; resume Install instead")
    python = str(venv_python())
    environment = dict(os.environ)
    environment["JAMTASTER_COMPONENT_ROOT"] = str(COMPONENT_ROOT)
    _install_progress("Checking which JamTaster parts need repair", 5)
    health = component_health()
    if health["healthy"]:
        _install_progress("JamTaster health is OK; no repair was needed", 100)
        return

    missing_dependencies = [
        name for name, available in health["dependencies"].items() if not available
    ]
    runtime_incompatible = not health["runtime_compatible"]
    if runtime_incompatible or any(
        name in {"torch", "torchaudio"} for name in missing_dependencies
    ):
        _install_progress("Repairing the private PyTorch runtime", 20)
        _run(
            _torch_install_command(python, cpu),
            environment=environment,
            activity="Downloading the required PyTorch runtime files",
            progress_range=(20, 48),
        )

    non_torch_missing = [
        name for name in missing_dependencies if name not in {"torch", "torchaudio"}
    ]
    if non_torch_missing:
        _install_progress(
            "Repairing missing analysis libraries: " + ", ".join(non_torch_missing), 50
        )
        # Pip's resolver leaves satisfied pinned packages untouched and downloads
        # only packages absent from this private component runtime.
        _run([
            python, "-m", "pip", "install", "--disable-pip-version-check",
            "--progress-bar", "raw", "-r", str(SOURCE_ROOT / "requirements.txt"),
        ], environment=environment, activity="Downloading missing analysis libraries",
            progress_range=(50, 72))

    missing_models = [
        name for name, available in health["models"].items() if not available
    ]
    if missing_models:
        _install_progress("Downloading missing JamTaster model files", 74)
        fetch_models("cpu" if cpu else resolve_device("auto"))

    device = "cpu" if cpu else resolve_device("auto")
    write_component_manifest("development", runtime_variant(device))
    repaired = component_health()
    if not repaired["healthy"]:
        remaining = [
            name for group in ("dependencies", "models")
            for name, available in repaired[group].items() if not available
        ]
        if not repaired["runtime_compatible"]:
            remaining.append("runtime compatibility")
        raise RuntimeError(
            "repair completed but health still reports: " + ", ".join(remaining)
        )
    _install_progress("JamTaster repair complete", 100)


def _validate_analysis_options(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if args.bass_min_midi > args.bass_max_midi:
        parser.error("--bass-min-midi cannot exceed --bass-max-midi")
    if not 0.0 < args.drum_candidate_scale <= 1.0:
        parser.error("--drum-candidate-scale must be above zero and at most one")
    if not 0.0 < args.drum_ghost_energy_ratio < args.drum_accent_energy_ratio:
        parser.error("drum ghost energy ratio must be positive and below accent ratio")
    if args.drum_repair_min_repeats < 1 or args.drum_repair_neighborhood_bars < 1:
        parser.error("drum repair repetition and neighborhood values must be positive")
    if args.local_drift_warning_ms < 0.0 or args.local_warp_threshold_ms < 0.0:
        parser.error("local drift values cannot be negative")


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args = parser.parse_args(raw)
    try:
        if args.command == "install":
            install_component(args.cpu)
            return 0
        if not _inside_runtime() and os.environ.get("JAMTASTER_RUNTIME_ACTIVE") != "1":
            return _reexec_in_runtime(raw)
        configure_local_caches()
        if args.command == "repair":
            repair_component(args.cpu)
            return 0
        if args.command == "models" and args.models_command == "fetch":
            device = resolve_device(args.device)
            fetch_models(device)
            write_component_manifest(
                "development" if not bool(getattr(sys, "frozen", False)) else "packaged",
                runtime_variant(device),
            )
            return 0
        if args.command == "doctor":
            health = component_health(_install_progress if args.progress else None)
            if args.json:
                print(json.dumps(health, ensure_ascii=False, separators=(",", ":")))
            else:
                print(json.dumps(health, indent=2, ensure_ascii=False))
            return 0 if health["healthy"] else 1
        if args.command == "worker":
            request_path = Path(args.request).expanduser().resolve()
            request = json.loads(request_path.read_text(encoding="utf-8"))
            from .jobs import run_request
            json_line_event({"type": "started", "action": request.get("action", "")})
            result = run_request(request, json_line_event)
            json_line_event({"type": "result", "result": result})
            return 0
        if args.command == "taste":
            _validate_analysis_options(args, parser)
            args.event_sink = json_line_event
            from .pipeline import taste
            output = taste(args)
            print(output)
            return 0
        parser.error("unknown command")
    except (RuntimeError, ValueError, FileExistsError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as exc:
        if args.command == "worker":
            json_line_event({"type": "error", "message": str(exc)})
        else:
            print(f"JamTaster error: {exc}", file=sys.stderr)
        return 1
    return 0
