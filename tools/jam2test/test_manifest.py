from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from jam2test.benchmark import _RunLog
from jam2test.manifest import InvocationManifest, sha256


class InvocationManifestTests(unittest.TestCase):
    def test_refresh_artifacts_replaces_stale_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "run.log"
            artifact.write_text("before\n", encoding="utf-8")
            manifest = InvocationManifest(
                root / "invocation-manifest.json", "benchmark", "test-run", []
            )
            manifest.finish("passed", 0)
            first = next(item for item in manifest.data["artifacts"] if item["path"] == "run.log")

            artifact.write_text("after\n", encoding="utf-8")
            manifest.refresh_artifacts()
            manifest.write()
            second = next(item for item in manifest.data["artifacts"] if item["path"] == "run.log")

            self.assertNotEqual(first["sha256"], second["sha256"])
            self.assertEqual(second["sha256"], sha256(artifact))
            self.assertEqual(second["bytes"], artifact.stat().st_size)

    def test_run_log_is_immutable_after_freeze(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "coordinator.log"
            log = _RunLog(path)
            self.assertTrue(path.is_file())
            with contextlib.redirect_stdout(io.StringIO()):
                log("retained")
                log.freeze()
                log("console only")

            text = path.read_text(encoding="utf-8")
            self.assertIn("retained", text)
            self.assertNotIn("console only", text)


if __name__ == "__main__":
    unittest.main()
