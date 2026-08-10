#!/usr/bin/env python3
"""JamTaster's single public command-line entry point."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from jamtaster.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
