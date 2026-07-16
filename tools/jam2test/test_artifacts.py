import tempfile
import unittest
from pathlib import Path

from jam2test.artifacts import (
    allocate_invocation, benchmark_attempt_path, normalized_path_id,
    validate_native_attempt_root,
)


class ArtifactTests(unittest.TestCase):
    def test_families_are_isolated_and_invocations_are_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            first = allocate_invocation("validate", tools, parent, "a")
            second = allocate_invocation("validate", tools, parent, "a")
            stress = allocate_invocation("stress", tools, parent, "a")
            self.assertNotEqual(first.root, second.root)
            self.assertEqual(first.family_root.parent, stress.family_root.parent)
            self.assertNotEqual(first.family_root, stress.family_root)

    def test_clean_removes_only_selected_family(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            validation = allocate_invocation("validate", tools, parent, "keep")
            stress = allocate_invocation("stress", tools, parent, "old")
            marker = validation.root / "marker"
            marker.write_text("keep")
            replacement = allocate_invocation("stress", tools, parent, "new", clean=True)
            self.assertTrue(marker.exists())
            self.assertFalse(stress.root.exists())
            self.assertTrue(replacement.root.exists())

    def test_validate_clean_removes_selected_validate_root_only(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            validation = allocate_invocation("validate", tools, parent, "old")
            stress = allocate_invocation("stress", tools, parent, "keep")
            validation_marker = validation.root / "old-evidence"
            stress_marker = stress.root / "retained-evidence"
            validation_marker.write_text("old")
            stress_marker.write_text("keep")
            replacement = allocate_invocation(
                "validate", tools, parent, "new", clean=True)
            self.assertFalse(validation.root.exists())
            self.assertFalse(validation_marker.exists())
            self.assertTrue(stress_marker.exists())
            self.assertTrue(replacement.root.exists())

    def test_benchmark_attempt_uses_normalized_nested_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            invocation = allocate_invocation("benchmark", tools, parent, "coordinator")
            attempt = benchmark_attempt_path(
                invocation, "0123456789ab", "machine-a", "fast_silence",
                "run-001", "abcdef012345",
            )
            self.assertTrue(attempt.is_dir())
            self.assertEqual(
                attempt.relative_to(invocation.root).as_posix(),
                "suites/0123456789ab/machines/machine-a/cases/fast_silence/"
                "runs/run-001/attempts/abcdef012345",
            )

    def test_long_identity_components_are_stably_shortened(self):
        value = "machine-with-a-name-that-would-overflow-a-deep-windows-path"
        first = normalized_path_id(value)
        self.assertEqual(first, normalized_path_id(value))
        self.assertLessEqual(len(first), 24)
        self.assertNotEqual(first, normalized_path_id(value + "-other"))

    def test_native_attempt_paths_have_a_global_length_bound(self):
        with self.assertRaisesRegex(ValueError, "shorter --output"):
            validate_native_attempt_root(Path("C:/") / ("x" * 2049))

    def test_output_parent_has_a_global_length_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            tools = Path(directory) / "repo" / "tools"
            tools.mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "2048-character bound"):
                allocate_invocation("validate", tools, Path("C:/") / ("x" * 2049))


if __name__ == "__main__":
    unittest.main()
