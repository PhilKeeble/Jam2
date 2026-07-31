from __future__ import annotations

import struct
import unittest

from tools.drum_research.analyze_gmd import (
    MidiNote,
    NOTE_IDENTITIES,
    ParsedMidi,
    parse_midi,
    select_grid,
    sequence_features,
)


def vlq(value: int) -> bytes:
    encoded = bytearray([value & 0x7F])
    value >>= 7
    while value:
        encoded.insert(0, 0x80 | (value & 0x7F))
        value >>= 7
    return bytes(encoded)


def midi_file(track: bytes, division: int = 480) -> bytes:
    return (
        b"MThd"
        + struct.pack(">IHHH", 6, 0, 1, division)
        + b"MTrk"
        + struct.pack(">I", len(track))
        + track
    )


class AnalyzeGmdTests(unittest.TestCase):
    def test_parser_preserves_articulation_velocity_and_hat_control(self) -> None:
        track = b"".join(
            (
                vlq(0),
                bytes((0x99, 36, 111)),
                vlq(120),
                bytes((0x99, 53, 87)),
                vlq(0),
                bytes((0xB9, 4, 63)),
                vlq(0),
                bytes((0xFF, 0x2F, 0)),
            )
        )
        parsed = parse_midi(midi_file(track))
        self.assertEqual(parsed.division, 480)
        self.assertEqual(
            [(item.tick, item.pitch, item.velocity) for item in parsed.notes],
            [(0, 36, 111), (120, 53, 87)],
        )
        self.assertEqual(parsed.hi_hat_controls, ((120, 63),))
        self.assertEqual(NOTE_IDENTITIES[53].articulation, "bell")

    def test_running_status_is_supported(self) -> None:
        track = b"".join(
            (
                vlq(0),
                bytes((0x99, 38, 100)),
                vlq(240),
                bytes((40, 70)),
                vlq(0),
                bytes((0xFF, 0x2F, 0)),
            )
        )
        parsed = parse_midi(midi_file(track))
        self.assertEqual(
            [(item.tick, item.pitch, item.velocity) for item in parsed.notes],
            [(0, 38, 100), (240, 40, 70)],
        )

    def test_grid_selection_ignores_common_anchors(self) -> None:
        selected, straight, triplet = select_grid(
            [0.0, 0.0, 0.01, -0.01],
            [0.0, 0.0, 0.01, -0.01],
        )
        self.assertEqual(selected, "straight-sixteenth")
        self.assertEqual((straight, triplet), (0, 0))

    def test_grid_selection_detects_clear_triplet_positions(self) -> None:
        selected, straight, triplet = select_grid(
            [0.08, -0.08, 0.075, -0.075],
            [0.003, -0.004, 0.005, -0.003],
        )
        self.assertEqual(selected, "triplet-sixth")
        self.assertEqual((straight, triplet), (0, 4))

    def test_grid_selection_requires_more_than_incidental_triplet_evidence(
        self,
    ) -> None:
        selected, _, triplet = select_grid(
            [0.08, 0.001],
            [0.001, 0.001],
        )
        self.assertEqual(triplet, 1)
        self.assertEqual(selected, "straight-sixteenth")

    def test_sequence_features_measure_recall_and_development(self) -> None:
        notes = (
            MidiNote(0, 36, 100),
            MidiNote(480, 38, 105),
            MidiNote(1920, 36, 98),
            MidiNote(2400, 38, 108),
            MidiNote(3840, 36, 102),
            MidiNote(4320, 38, 110),
            MidiNote(4560, 42, 62),
            MidiNote(5760, 36, 96),
            MidiNote(6240, 38, 106),
        )
        midi = ParsedMidi(
            division=480,
            notes=notes,
            final_tick=6720,
            hi_hat_controls=(),
        )
        known = [
            (note, NOTE_IDENTITIES[note.pitch])
            for note in notes
        ]
        result = sequence_features(
            midi, known, "straight-sixteenth", 4, 4
        )
        self.assertEqual(result["activeBars"], 4)
        self.assertEqual(result["uniqueCoreBarRatio"], 0.5)
        self.assertEqual(result["period2CoreIdenticalRatio"], 0.5)
        self.assertEqual(result["period4CoreIdenticalRatio"], 0.0)
        self.assertEqual(len(result["barHitCounts"]), 4)


if __name__ == "__main__":
    unittest.main()
