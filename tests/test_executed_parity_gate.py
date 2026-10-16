import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from executed_parity_gate import ParityError, compare

BLOB = "a0b3fc658d1460eab62c85b3cc897143ea77a7ff"
OBSERVED = {
    "main()": 43, "counter_response(-1)": -43, "counter_response(0)": 0,
}
OBSERVED.update({f"counter_response({i})": 43 * i for i in range(1, 129)})


def record(backend):
    return {
        "backend": backend,
        "fixture_blob": BLOB,
        "source_tree": "1" * 40,
        "compiler_sha": "2" * 40,
        "run_id": 4000,
        "job_id": 9000,
        "runner_id": 20,
        "executed_tests": len(OBSERVED),
        "skipped_tests": 0,
        "failed_tests": 0,
        "errored_tests": 0,
        "conclusion": "success",
        "observed": dict(OBSERVED),
    }


class ExecutedParityProofTest(unittest.TestCase):
    def test_real_result_records_match(self):
        self.assertEqual(131, compare(record("graal"), record("llvm"), BLOB))

    def assert_reject(self, update, backend="llvm"):
        left, right = record("graal"), record("llvm")
        update(right if backend == "llvm" else left)
        with self.assertRaises(ParityError):
            compare(left, right, BLOB)

    def test_identically_wrong_result_is_not_parity(self):
        left, right = record("graal"), record("llvm")
        left["observed"]["counter_response(1)"] = 44
        right["observed"]["counter_response(1)"] = 44
        with self.assertRaisesRegex(ParityError, "expected 43"):
            compare(left, right, BLOB)

    def test_missing_edge_probe_rejected(self):
        self.assert_reject(lambda r: r["observed"].pop("counter_response(-1)"))

    def test_no_real_runner(self):
        self.assert_reject(lambda r: r.update(runner_id=0))

    def test_boolean_runner_is_not_integer_evidence(self):
        self.assert_reject(lambda r: r.update(runner_id=True))

    def test_skipped_test_is_not_success(self):
        self.assert_reject(lambda r: r.update(skipped_tests=1))

    def test_failed_test_is_not_success(self):
        self.assert_reject(lambda r: r.update(failed_tests=1))

    def test_divergent_guest_result(self):
        self.assert_reject(lambda r: r["observed"].update({"counter_response(2)": 87}))

    def test_different_case_set(self):
        self.assert_reject(lambda r: r["observed"].pop("counter_response(2)"))

    def test_blob_mismatch(self):
        self.assert_reject(lambda r: r.update(fixture_blob="3" * 40))

    def test_empty_results(self):
        self.assert_reject(lambda r: r.update(observed={}))

    def test_success_without_executed_tests(self):
        self.assert_reject(lambda r: r.update(executed_tests=0))

    def test_false_observation_not_interpreted_as_one(self):
        self.assert_reject(lambda r: r["observed"].update({"counter_response(1)": True}))

    def test_missing_provenance(self):
        self.assert_reject(lambda r: r.pop("compiler_sha"))


if __name__ == "__main__":
    unittest.main()
