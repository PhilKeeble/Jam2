import unittest

from jam2test.profiles import FAST_PROFILE, configure_native_profiles, variant
from jam2test.native import NativeCapabilities
from jam2test.scenarios import expand_scenarios


class ProfileTests(unittest.TestCase):
    def test_asymmetric_profile_comparison_expands_both_roles_and_control(self):
        cases = expand_scenarios(["asymmetric-profile-comparison"])
        self.assertEqual(6, len(cases))
        self.assertIn("asymmetric-fast-create-safe-join-clean", cases)
        self.assertIn("asymmetric-safe-create-fast-join-pressure", cases)
        self.assertIn("asymmetric-safe-create-safe-join-pressure", cases)

    def test_native_base_and_sparse_override_are_distinct(self):
        configure_native_profiles({"profiles": [{"name": "fast", "frame_size": 64,
                                                   "playback_ring_frames": 4096}]})
        changed = variant(FAST_PROFILE, "frame", frame_size=128)
        self.assertEqual(64, FAST_PROFILE.frame_size)
        self.assertEqual(128, changed.frame_size)
        self.assertEqual({"frame_size": 128}, changed.overrides)
        self.assertNotIn("playback_ring_frames", changed.overrides)

    def test_sparse_overrides_use_native_described_types_and_bounds(self):
        capabilities = NativeCapabilities.__new__(NativeCapabilities)
        capabilities.runtime_fields = {
            "frame_size": {"type": "integer", "minimum": 32, "maximum": 256},
            "sample_rate": {"type": "integer", "minimum": 8000, "maximum": 384000},
            "metronome_mode": {"type": "string", "choices": ["shared-grid", "leader-audio"]},
            "os_priority": {"type": "string", "choices": ["off", "high", "realtime"]},
        }
        capabilities.validate_sparse_overrides({
            "frame_size": 128, "sample_rate": 8000,
            "metronome_mode": "shared-grid", "os_priority": "off",
        })
        capabilities.validate_sparse_overrides({"sample_rate": 384000})
        with self.assertRaises(ValueError):
            capabilities.validate_sparse_overrides({"frame_size": 512})
        with self.assertRaises(ValueError):
            capabilities.validate_sparse_overrides({"metronome_mode": "unknown"})
        with self.assertRaises(ValueError):
            capabilities.validate_sparse_overrides({"sample_rate": 7999})
        with self.assertRaises(ValueError):
            capabilities.validate_sparse_overrides({"sample_rate": 384001})


if __name__ == "__main__":
    unittest.main()
