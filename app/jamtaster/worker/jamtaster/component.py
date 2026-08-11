from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import sys
from typing import Any, Callable

from . import __version__
from .models import ADTOF_REVISION, CHORDMINI_REVISION
from .paths import COMPONENT_ROOT, MANIFEST_PATH, MODELS_ROOT, PROTOCOL_VERSION, SOURCE_ROOT, venv_python


def runtime_variant(device: str) -> str:
    if sys.platform == "darwin":
        return "apple"
    return "cuda" if device.startswith("cuda") else "cpu"


def component_manifest(
    mode: str = "development", accelerator: str = "cpu"
) -> dict[str, Any]:
    return {
        "format": "jamtaster-component-v1",
        "version": __version__,
        "protocol": PROTOCOL_VERSION,
        "mode": mode,
        "accelerator": accelerator,
        "installed_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "python_version": platform.python_version(),
        "python": str(venv_python()),
        "entry_point": str(SOURCE_ROOT / "JamTaster.py"),
        "models_root": str(MODELS_ROOT),
        "model_revisions": {
            "chordmini": CHORDMINI_REVISION,
            "adtof": ADTOF_REVISION,
        },
    }


def write_component_manifest(
    mode: str = "development", accelerator: str = "cpu"
) -> Path:
    COMPONENT_ROOT.mkdir(parents=True, exist_ok=True)
    temporary = MANIFEST_PATH.with_suffix(".json.partial")
    temporary.write_text(
        json.dumps(component_manifest(mode, accelerator), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(MANIFEST_PATH)
    return MANIFEST_PATH


def component_health(
    progress: Callable[[str, int], None] | None = None,
) -> dict[str, Any]:
    report = progress or (lambda _message, _percent: None)
    report("Inspecting JamTaster runtime files", 15)
    dependencies = {
        name: _module_available(module)
        for name, module in {
            "torch": "torch",
            "torchaudio": "torchaudio",
            "demucs": "demucs_infer",
            "beat_this": "beat_this",
            "basic_pitch": "basic_pitch",
            "adtof": "adtof_pytorch",
            "signalsmith": "python_stretch",
        }.items()
    }
    report("Checking pinned model files", 45)
    torch_checkpoints = COMPONENT_ROOT / "cache" / "torch" / "hub" / "checkpoints"
    demucs_weights = list(torch_checkpoints.glob("*.th")) if torch_checkpoints.is_dir() else []
    models = {
        "chordmini": (
            MODELS_ROOT / "chordmini" / "checkpoints" / "btc_model_best.pth"
        ).is_file(),
        "beat_this": (torch_checkpoints / "beat_this-final0.ckpt").is_file(),
        "demucs": len(demucs_weights) >= 4,
    }
    cuda_available = False
    cuda_compiled = False
    mps_available = False
    mps_compiled = False
    torch_version = ""
    default_device = "cpu"
    cuda_device = ""
    torch_compute_threads = 0
    torch_interop_threads = 0
    report("Loading the private PyTorch runtime", 65)
    try:
        import torch
        torch_version = str(torch.__version__)
        torch_compute_threads = int(torch.get_num_threads())
        torch_interop_threads = int(torch.get_num_interop_threads())
        cuda_compiled = bool(torch.version.cuda)
        cuda_available = bool(torch.cuda.is_available())
        if cuda_available:
            default_device = "cuda"
            cuda_device = str(torch.cuda.get_device_name(0))
        mps = getattr(torch.backends, "mps", None)
        if mps is not None:
            mps_compiled = bool(mps.is_built())
            mps_available = bool(mps.is_available())
    except ImportError:
        pass
    installed = {}
    if MANIFEST_PATH.is_file():
        try:
            installed = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            installed = {}
    variant = str(installed.get("accelerator", "unknown"))
    runtime_compatible = (
        (variant == "cpu" and not cuda_compiled)
        or (variant == "cuda" and cuda_compiled and cuda_available)
        or (variant == "apple" and sys.platform == "darwin" and not cuda_compiled)
    )
    report("Finalising JamTaster health results", 90)
    healthy = all(dependencies.values()) and all(models.values()) and runtime_compatible
    return {
        "format": "jamtaster-health-v1",
        "version": __version__,
        "protocol": PROTOCOL_VERSION,
        "healthy": healthy,
        "component_root": str(COMPONENT_ROOT),
        "python": sys.executable,
        "dependencies": dependencies,
        "models": models,
        "runtime_variant": variant,
        "runtime_compatible": runtime_compatible,
        "torch_version": torch_version,
        "default_device": default_device,
        "device": cuda_device or "CPU",
        "cpu": {
            "logical_processors": int(os.cpu_count() or 1),
            "torch_compute_threads": torch_compute_threads,
            "torch_interop_threads": torch_interop_threads,
        },
        "cuda_available": cuda_available,
        "accelerators": {
            "cuda_compiled": cuda_compiled,
            "cuda_available": cuda_available,
            "mps_compiled": mps_compiled,
            "mps_available": mps_available,
        },
    }


def _module_available(name: str) -> bool:
    import importlib.util
    return importlib.util.find_spec(name) is not None
