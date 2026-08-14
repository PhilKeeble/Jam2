import csv
import tempfile
import unittest
from pathlib import Path

from jam2test.metrics import normalized_pair_summary, summarize_csv


class MetricReductionTests(unittest.TestCase):
    def test_pair_summary_uses_stable_machine_records(self):
        value = normalized_pair_summary(
            "machine-a", None, "machine-b", None)
        self.assertEqual(
            ["machine-a", "machine-b"],
            [peer["machine_id"] for peer in value["peers"]],
        )
        self.assertEqual(
            {"coordinator", "agent"},
            {peer["role"] for peer in value["peers"]},
        )
        self.assertNotIn("server", value)
        self.assertNotIn("client", value)

    def test_assessment_uses_last_periodic_row_before_teardown(self):
        fields = [
            "row_type",
            "elapsed_ms",
            "network_active_peer_count",
            "grid_mapped_epoch_frame",
            "metronome_alignment_valid",
            "metronome_epoch_sample_time",
            "local_metronome_beat",
            "remote_metronome_beat",
            "recv_packets",
        ]
        rows = [
            {
                "row_type": "periodic",
                "elapsed_ms": "20000",
                "network_active_peer_count": "1",
                "grid_mapped_epoch_frame": "148200",
                "metronome_alignment_valid": "yes",
                "metronome_epoch_sample_time": "148200",
                "local_metronome_beat": "3",
                "remote_metronome_beat": "3",
                "recv_packets": "1000",
            },
            {
                "row_type": "final",
                "elapsed_ms": "20634",
                "network_active_peer_count": "0",
                "grid_mapped_epoch_frame": "0",
                "metronome_alignment_valid": "no",
                "metronome_epoch_sample_time": "0",
                "local_metronome_beat": "0",
                "remote_metronome_beat": "0",
                "recv_packets": "1032",
            },
        ]

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            summary = summarize_csv(path, assessment_elapsed_ms=20000)

        self.assertEqual(0, summary["network_active_peer_count"])
        self.assertEqual(1032, summary["recv_packets"])
        self.assertEqual(148200, summary["grid_mapped_epoch_frame"])
        self.assertEqual("yes", summary["metronome_alignment_valid"])
        self.assertEqual(0, summary["metronome_beat_delta_abs"])


if __name__ == "__main__":
    unittest.main()
