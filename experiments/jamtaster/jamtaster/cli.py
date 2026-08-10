from __future__ import annotations

import argparse
import importlib.metadata
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import venv

from .models import (
    ADTOF_REVISION,
    CHORDMINI_REVISION,
    SONGFORMER_REVISION,
    configure_local_caches,
    fetch_models,
    resolve_device,
)
from .paths import CACHE_ROOT, EXPERIMENT_ROOT, MODELS_ROOT, VENV_ROOT, venv_python


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="JamTaster.py",
        description="Taste a Jam2 loopback WAV and create a copy-ready Jam2 song folder.",
    )
    parser.add_argument("--version", action="version", version="JamTaster 0.1.0")
    commands = parser.add_subparsers(dest="command", required=True)

    setup = commands.add_parser("setup", help="create the isolated .venv and install dependencies")
    setup.add_argument("--cpu", action="store_true", help="install CPU-only PyTorch")

    models = commands.add_parser("models", help="manage local model weights")
    model_commands = models.add_subparsers(dest="models_command", required=True)
    fetch = model_commands.add_parser("fetch", help="fetch all pinned source and model weights")
    fetch.add_argument("--device", default="auto", choices=("auto", "cpu", "cuda"))

    commands.add_parser("doctor", help="report dependency, model, and accelerator status")

    datasets = commands.add_parser(
        "datasets", help="inventory and benchmark ignored local reference corpora"
    )
    dataset_commands = datasets.add_subparsers(dest="datasets_command", required=True)
    dataset_commands.add_parser("inventory", help="validate labelled audio/reference pairs")
    reference = dataset_commands.add_parser(
        "benchmark", help="compare raw and context-processed analysis on labelled audio"
    )
    reference.add_argument("dataset", choices=("guitarset", "filobass", "mdb-drums"))
    reference.add_argument("--split", default="dev", choices=("dev", "heldout", "all"))
    reference.add_argument("--limit", type=int, default=5)
    reference.add_argument("--device", default="auto", choices=("auto", "cpu", "cuda"))

    benchmark = commands.add_parser(
        "benchmark", help="prepare a short known-truth mix from native Jam2 generated assets"
    )
    benchmark.add_argument("jamjar", help="native Jam2 .jamjar containing generated lanes")
    benchmark.add_argument("--name", required=True, help="unique benchmark report name")
    benchmark.add_argument("--bank", type=int, default=0, help="zero-based generated bank index")
    benchmark.add_argument("--bars", type=int, default=8, help="number of bars to mix")

    score = commands.add_parser("score", help="score a JamTaster report against benchmark truth")
    score.add_argument("truth", help="truth.json made by the benchmark command")
    score.add_argument("analysis", help="analysis.json made by the taste command")

    taste = commands.add_parser("taste", help="convert one Jam2 loopback WAV")
    taste.add_argument("input", help="mono PCM16 Jam2 loopback .wav")
    taste.add_argument("--name", required=True, help="Jam2 song name (also determines folder name)")
    taste.add_argument("--device", default="auto", choices=("auto", "cpu", "cuda"))
    taste.add_argument("--sections", default="auto", choices=("auto", "single"))
    taste.add_argument(
        "--bpm", type=float,
        help="override the fixed Jam2 tempo; exported stems are stretched to its exact grid",
    )
    taste.add_argument("--meter", default="auto", choices=("auto", *[str(value) for value in range(2, 13)]))
    taste.add_argument("--no-arrangement-loop", action="store_true")
    taste.add_argument(
        "--no-time-stretch", action="store_true",
        help="keep the original section durations instead of aligning stems to the Jam2 grid",
    )
    taste.add_argument("--demucs-model", default="htdemucs_ft")
    taste.add_argument("--beat-model", default="final0")
    taste.add_argument(
        "--drum-thresholds", default="0.12,0.16,0.12,0.05,0.20",
        help="ADTOF kick,snare,tom,hihat,cymbal thresholds",
    )
    taste.add_argument(
        "--drum-candidate-scale", type=float, default=0.30,
        help="permissive repair-candidate thresholds as a fraction of detection thresholds",
    )
    taste.add_argument("--drum-division", type=int, default=4, choices=(2, 3, 4, 6, 8))
    taste.add_argument("--drum-ghost-energy-ratio", type=float, default=0.50)
    taste.add_argument("--drum-accent-energy-ratio", type=float, default=0.85)
    taste.add_argument("--drum-repair-min-repeats", type=int, default=3)
    taste.add_argument("--drum-repair-neighborhood-bars", type=int, default=8)
    taste.add_argument("--no-drum-repair", action="store_true")
    taste.add_argument("--bass-min-midi", type=int, default=28)
    taste.add_argument("--bass-max-midi", type=int, default=60)
    taste.add_argument(
        "--drift-warning-ms", type=float, default=100.0,
        help="report audio-versus-fixed-grid drift above this magnitude",
    )
    taste.add_argument(
        "--local-drift-warning-ms", type=float, default=45.0,
        help="report internal beat drift hidden by endpoint-only section stretching",
    )
    taste.add_argument(
        "--local-warp-threshold-ms", type=float, default=45.0,
        help="use tracked-bar Signalsmith warping above this internal residual",
    )
    return parser


def _inside_venv() -> bool:
    try:
        return Path(sys.prefix).resolve() == VENV_ROOT.resolve()
    except OSError:
        return False


def _reexec_in_venv(argv: list[str]) -> int:
    python = venv_python()
    if not python.is_file():
        raise RuntimeError("JamTaster is not set up; run: python JamTaster.py setup")
    environment = dict(os.environ)
    environment["JAMTASTER_VENV_ACTIVE"] = "1"
    completed = subprocess.run(
        [str(python), str(EXPERIMENT_ROOT / "JamTaster.py"), *argv], env=environment, check=False
    )
    return completed.returncode


def _run(command: list[str]) -> None:
    print(" ".join(command))
    subprocess.run(command, check=True, cwd=EXPERIMENT_ROOT)


def setup_environment(cpu: bool) -> None:
    configure_local_caches()
    if sys.version_info[:2] != (3, 10):
        raise RuntimeError(
            f"this POC requires Python 3.10; current interpreter is {sys.version_info.major}.{sys.version_info.minor}"
        )
    if VENV_ROOT.exists() and not venv_python().is_file():
        raise RuntimeError(f"{VENV_ROOT} exists but is not a usable virtual environment")
    if not venv_python().is_file():
        print(f"Creating isolated environment at {VENV_ROOT}...")
        venv.EnvBuilder(with_pip=True).create(VENV_ROOT)
    python = str(venv_python())
    _run([python, "-m", "pip", "install", "--upgrade", "pip==25.1.1", "setuptools==80.9.0", "wheel==0.45.1"])
    torch_index = "https://download.pytorch.org/whl/cpu" if cpu else "https://download.pytorch.org/whl/cu128"
    _run([
        python, "-m", "pip", "install", "--index-url", torch_index,
        "torch==2.9.1", "torchaudio==2.9.1",
    ])
    _run([python, "-m", "pip", "install", "-r", str(EXPERIMENT_ROOT / "requirements.txt")])
    print("Environment ready. Next run: python JamTaster.py models fetch")


def doctor() -> int:
    configure_local_caches()
    print(f"python={sys.executable}")
    print(f"venv={VENV_ROOT} active={_inside_venv()}")
    print(f"models={MODELS_ROOT}")
    failed = False
    packages = {
        "torch": "torch", "torchaudio": "torchaudio", "demucs-infer": "demucs_infer",
        "beat-this": "beat_this", "transformers": "transformers", "basic-pitch": "basic_pitch",
        "adtof-pytorch": "adtof_pytorch", "librosa": "librosa", "msaf": "msaf", "vmo": "vmo",
        "python-stretch": "python_stretch",
    }
    for distribution, module in packages.items():
        present = importlib.util.find_spec(module) is not None
        try:
            version = importlib.metadata.version(distribution) if present else "missing"
        except importlib.metadata.PackageNotFoundError:
            version = "present (unversioned)" if present else "missing"
        print(f"dependency.{distribution}={version}")
        failed |= not present
    try:
        import torch
        print(f"torch.cuda_available={torch.cuda.is_available()}")
        if torch.cuda.is_available():
            print(f"torch.cuda_device={torch.cuda.get_device_name(0)}")
    except ImportError:
        pass
    model_checks = {
        "songformer": (MODELS_ROOT / "songformer" / "config.json").is_file(),
        "chordmini": (
            (MODELS_ROOT / "chordmini" / "checkpoints" / "btc_model_best.pth").is_file()
            and (MODELS_ROOT / "chordmini" / "checkpoints" / "btc_model_best.pth").stat().st_size
            > 1024 * 1024
        ),
        "demucs_cache": any(
            (CACHE_ROOT / "torch" / "hub" / "checkpoints").glob("*.th")
        ) if (CACHE_ROOT / "torch" / "hub" / "checkpoints").is_dir() else False,
    }
    for name, present in model_checks.items():
        print(f"model.{name}={'ready' if present else 'missing'}")
        failed |= not present
    print(f"pin.songformer={SONGFORMER_REVISION}")
    print(f"pin.chordmini={CHORDMINI_REVISION}")
    print(f"pin.adtof={ADTOF_REVISION}")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args = parser.parse_args(raw)
    try:
        if args.command == "setup":
            setup_environment(args.cpu)
            return 0
        if not _inside_venv() and os.environ.get("JAMTASTER_VENV_ACTIVE") != "1":
            return _reexec_in_venv(raw)
        if args.command == "doctor":
            return doctor()
        if args.command == "datasets":
            from .datasets import run_benchmark, write_inventory
            if args.datasets_command == "inventory":
                output = write_inventory()
                print(output.read_text(encoding="utf-8"), end="")
                print(f"Wrote dataset inventory: {output}")
                return 0
            if args.limit < 1:
                parser.error("--limit must be at least one")
            output = run_benchmark(
                args.dataset, args.split, args.limit, resolve_device(args.device)
            )
            print(f"Wrote reference benchmark: {output}")
            return 0
        if args.command == "benchmark":
            from .benchmark import prepare_generated_benchmark
            mix, truth = prepare_generated_benchmark(
                Path(args.jamjar), args.name.strip(), args.bank, args.bars
            )
            print(f"Prepared known-truth full mix: {mix}")
            print(f"Ground truth: {truth}")
            return 0
        if args.command == "score":
            from .benchmark import score_generated_benchmark
            output = score_generated_benchmark(Path(args.truth), Path(args.analysis))
            print(f"Wrote benchmark score: {output}")
            return 0
        if args.command == "models" and args.models_command == "fetch":
            fetch_models(resolve_device(args.device))
            return 0
        if args.command == "taste":
            if args.bass_min_midi > args.bass_max_midi:
                parser.error("--bass-min-midi cannot exceed --bass-max-midi")
            if not 0.0 < args.drum_candidate_scale <= 1.0:
                parser.error("--drum-candidate-scale must be above zero and at most one")
            if not 0.0 < args.drum_ghost_energy_ratio < args.drum_accent_energy_ratio:
                parser.error("drum ghost energy ratio must be positive and below accent ratio")
            if args.drum_repair_min_repeats < 1:
                parser.error("--drum-repair-min-repeats must be at least one")
            if args.drum_repair_neighborhood_bars < 1:
                parser.error("--drum-repair-neighborhood-bars must be at least one")
            if args.local_drift_warning_ms < 0.0:
                parser.error("--local-drift-warning-ms cannot be negative")
            if args.local_warp_threshold_ms < 0.0:
                parser.error("--local-warp-threshold-ms cannot be negative")
            from .pipeline import taste
            output = taste(args)
            print(f"Created copy-ready Jam2 song: {output}")
            print(f"Copy that entire folder into the songs folder next to jam2.exe.")
            return 0
        parser.error("unknown command")
    except (RuntimeError, ValueError, FileExistsError, subprocess.CalledProcessError) as exc:
        print(f"JamTaster error: {exc}", file=sys.stderr)
        return 1
    return 0
