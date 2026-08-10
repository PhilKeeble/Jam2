from __future__ import annotations

from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class WavInfo:
    path: Path
    sample_rate: int
    frames: int
    channels: int
    sample_width: int

    @property
    def duration(self) -> float:
        return self.frames / self.sample_rate


@dataclass(frozen=True)
class TimedLabel:
    start: float
    end: float
    label: str
    confidence: float | None = None


@dataclass(frozen=True)
class NoteEvent:
    start: float
    end: float
    midi: int
    velocity: int = 88
    confidence: float | None = None


@dataclass(frozen=True)
class DrumHit:
    time: float
    lane: str
    velocity: int = 100
    confidence: float | None = None
    energy_ratio: float | None = None
    provenance: str = "detected"


@dataclass
class Analysis:
    beats: list[float]
    downbeats: list[float]
    bpm: float
    beats_per_bar: int
    structures: list[TimedLabel]
    chords: list[TimedLabel]
    drums: list[DrumHit]
    bass: list[NoteEvent]
    drum_candidates: list[DrumHit] = field(default_factory=list)
    drum_repair: list[dict[str, Any]] = field(default_factory=list)
    drum_division: int = 4
    raw: dict[str, Any] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class SectionChoice:
    role: str
    source_label: str
    start: float
    end: float
    first_beat: int
    beats: int


def jsonable(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, Path):
        return str(value)
    if hasattr(value, "__dataclass_fields__"):
        return {key: jsonable(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(item) for item in value]
    # Model outputs commonly contain NumPy scalar/array values. Keep NumPy an
    # optional dependency of this small data module by using its public scalar
    # conversion conventions instead of importing it here.
    item = getattr(value, "item", None)
    if callable(item):
        try:
            return jsonable(item())
        except (TypeError, ValueError):
            pass
    tolist = getattr(value, "tolist", None)
    if callable(tolist):
        return jsonable(tolist())
    return value
