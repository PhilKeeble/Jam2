import unittest

from jam2test.benchmark import _agent_attempt_status, _correlated_process_outcome


class BenchmarkOutcomeTests(unittest.TestCase):
    def test_correlated_attempt_completes_only_when_both_processes_succeed(self):
        self.assertEqual(("complete", True), _correlated_process_outcome(0, {"return_code": 0}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(4, {"return_code": 0}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(0, {"return_code": 4}))
        self.assertEqual(("process-failed", False), _correlated_process_outcome(0, {}))

    def test_agent_attempt_requires_upload_and_process_success(self):
        self.assertEqual("passed", _agent_attempt_status(True, 0))
        self.assertEqual("failed", _agent_attempt_status(True, 4))
        self.assertEqual("failed", _agent_attempt_status(False, 0))


if __name__ == "__main__":
    unittest.main()
