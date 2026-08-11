#!/usr/bin/env python3
"""JamTaster's single command-line and Jam2 worker entry point."""

from pathlib import Path
import os
import sys


# The component root controls every shared runtime/model/cache path. Read this
# one bootstrap option before importing the package, whose paths are immutable
# for the lifetime of the worker process.
for index, value in enumerate(sys.argv[:-1]):
    if value == "--component-root":
        os.environ["JAMTASTER_COMPONENT_ROOT"] = sys.argv[index + 1]
        break

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from jamtaster.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
