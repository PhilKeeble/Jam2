from __future__ import annotations

import json
import sys
import time
from typing import Any, Callable


EventSink = Callable[[dict[str, Any]], None]


def json_line_event(event: dict[str, Any]) -> None:
    payload = dict(event)
    payload.setdefault("protocol", 1)
    payload.setdefault("timestamp", time.time())
    print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")), flush=True)


def text_event(event: dict[str, Any]) -> None:
    stage = str(event.get("stage", "jamtaster"))
    message = str(event.get("message", event.get("type", "progress")))
    print(f"[{stage}] {message}", file=sys.stderr, flush=True)
