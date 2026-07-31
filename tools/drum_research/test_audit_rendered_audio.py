import struct
import tempfile
import unittest
import wave
from pathlib import Path

from tools.drum_research.audit_rendered_audio import audit, inspect_wav


class RenderedAudioAuditTest(unittest.TestCase):
    def test_reports_pcm_signal_measurements(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "pop_loop__test.wav"
            values = [0, 8192, -16384, 32112]
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(8000)
                output.writeframes(
                    struct.pack(f"<{len(values)}h", *values)
                )
            result = inspect_wav(path)
            self.assertEqual(result["profile"], "pop_loop")
            self.assertAlmostEqual(result["peak"], 0.97998, places=5)
            self.assertEqual(result["nearLimitSamples"], 1)
            report = audit(Path(folder))
            self.assertEqual(report["overall"]["files"], 1)
            self.assertIn("pop_loop", report["byProfile"])


if __name__ == "__main__":
    unittest.main()
