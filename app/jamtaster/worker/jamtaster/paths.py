from __future__ import annotations

import os
from pathlib import Path
import re
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[1]
COMPONENT_VERSION = "1.0.0"
PROTOCOL_VERSION = 1


def default_component_root() -> Path:
    override = os.environ.get("JAMTASTER_COMPONENT_ROOT", "").strip()
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return base / "Jam2" / "components" / "jamtaster" / COMPONENT_VERSION
    if sys.platform == "darwin":
        return (
            Path.home()
            / "Library"
            / "Application Support"
            / "Jam2"
            / "components"
            / "jamtaster"
            / COMPONENT_VERSION
        )
    return Path.home() / ".local" / "share" / "Jam2" / "components" / "jamtaster" / COMPONENT_VERSION


COMPONENT_ROOT = default_component_root()
VENV_ROOT = COMPONENT_ROOT / "runtime"
CACHE_ROOT = COMPONENT_ROOT / "cache"
MODELS_ROOT = COMPONENT_ROOT / "models"
MANIFEST_PATH = COMPONENT_ROOT / "component.json"


def venv_python() -> Path:
    if os.name == "nt":
        return VENV_ROOT / "Scripts" / "python.exe"
    return VENV_ROOT / "bin" / "python"


def project_analysis_root(project_root: Path) -> Path:
    return project_root.expanduser().resolve() / "analysis"


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
