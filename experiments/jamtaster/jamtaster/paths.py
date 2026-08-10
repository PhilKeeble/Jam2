from __future__ import annotations

import os
from pathlib import Path
import re


EXPERIMENT_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = EXPERIMENT_ROOT.parents[1]
DATASETS_ROOT = REPOSITORY_ROOT / "datasets"
VENV_ROOT = EXPERIMENT_ROOT / ".venv"
CACHE_ROOT = EXPERIMENT_ROOT / ".cache"
MODELS_ROOT = EXPERIMENT_ROOT / "models"
SONGS_ROOT = EXPERIMENT_ROOT / "songs"
REPORTS_ROOT = EXPERIMENT_ROOT / "reports"


def venv_python() -> Path:
    if os.name == "nt":
        return VENV_ROOT / "Scripts" / "python.exe"
    return VENV_ROOT / "bin" / "python"


def portable_slug(display_name: str) -> str:
    """Match JamStorage::portableSlug, including Windows device names."""
    value = display_name.strip()
    value = re.sub(r"\s+", "_", value)
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", value)
    value = re.sub(r"_+", "_", value)
    value = re.sub(r"[. _]+$", "", value)
    value = value.lstrip("._")
    if not value:
        value = "Untitled_Jam"
    if re.fullmatch(r"(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])", value):
        value = "_" + value
    return value[:120]
