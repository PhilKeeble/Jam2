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
            first = allocate_invocation("fuzz", tools)
            second = allocate_invocation("fuzz", tools)
            connectivity = allocate_invocation("connectivity", tools)
            self.assertNotEqual(first.root, second.root)
            self.assertEqual(
                first.family_root.parent, connectivity.family_root.parent)
            self.assertNotEqual(first.family_root, connectivity.family_root)
            self.assertRegex(first.invocation_id, r"^\d{8}T\d{4}Z(?:_\d+)?$")

    def test_custom_output_is_the_exact_timestamp_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            output = parent / "artifacts" / "localhost_headless_coord"
            invocation = allocate_invocation(
                "benchmark", tools, output, "coordinator")
            self.assertEqual(invocation.family_root, output.resolve())
            self.assertEqual(invocation.root.parent, output.resolve())
            self.assertNotIn("benchmark_logs", invocation.root.parts)

    def test_clean_removes_only_selected_family(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            benchmark_output = parent / "benchmark"
            fuzz_output = parent / "fuzz"
            benchmark = allocate_invocation(
                "benchmark", tools, benchmark_output, "keep")
            fuzz = allocate_invocation("fuzz", tools, fuzz_output, "old")
            marker = benchmark.root / "marker"
            marker.write_text("keep")
            replacement = allocate_invocation(
                "fuzz", tools, fuzz_output, "new", clean=True)
            self.assertTrue(marker.exists())
            self.assertEqual(fuzz.root, replacement.root)
            self.assertTrue(replacement.root.exists())

    def test_default_clean_removes_selected_fuzz_root_only(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            fuzz = allocate_invocation("fuzz", tools, run_id="old")
            connectivity = allocate_invocation(
                "connectivity", tools, run_id="keep")
            fuzz_marker = fuzz.root / "old-evidence"
            connectivity_marker = connectivity.root / "retained-evidence"
            fuzz_marker.write_text("old")
            connectivity_marker.write_text("keep")
            replacement = allocate_invocation(
                "fuzz", tools, run_id="new", clean=True)
            self.assertFalse(fuzz_marker.exists())
            self.assertEqual(fuzz.family_root, replacement.family_root)
            self.assertTrue(connectivity_marker.exists())
            self.assertTrue(replacement.root.exists())

    def test_benchmark_attempt_uses_normalized_nested_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            tools = parent / "repo" / "tools"
            tools.mkdir(parents=True)
            invocation = allocate_invocation("benchmark", tools, parent, "coordinator")
            attempt = benchmark_attempt_path(
                invocation, "0123456789ab", "coordinator", "fast_silence",
                "1", "abcdef012345",
            )
            self.assertTrue(attempt.is_dir())
            self.assertEqual(
                attempt.relative_to(invocation.root).as_posix(),
                "fast_silence/1",
            )

    def test_long_identity_components_are_stably_shortened(self):
        value = "case-with-a-readable-name-" + ("x" * 80)
        first = normalized_path_id(value)
        self.assertEqual(first, normalized_path_id(value))
        self.assertLessEqual(len(first), 64)
        self.assertNotEqual(first, normalized_path_id(value + "-other"))

    def test_native_attempt_paths_have_a_global_length_bound(self):
        with self.assertRaisesRegex(ValueError, "shorter --output"):
            validate_native_attempt_root(Path("C:/") / ("x" * 2049))

    def test_output_parent_has_a_global_length_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            tools = Path(directory) / "repo" / "tools"
            tools.mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "2048-character bound"):
                allocate_invocation("benchmark", tools, Path("C:/") / ("x" * 2049))


if __name__ == "__main__":
    unittest.main()
