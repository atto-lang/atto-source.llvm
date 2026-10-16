import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from http_guest_log_adapter import (
    FIXTURE_BLOB, LogAdapterError, normalize,
)

CASES = {
    "main()": 43, "counter_response(-1)": -43,
    "counter_response(0)": 0, "counter_response(1)": 43,
    "counter_response(2)": 86,
}
CASES.update({f"counter_response({i})": 43 * i for i in range(1, 129)})


def graal(values=None):
    return ("graal test started\nGRAAL_GUEST_SHARED_HANDLER_RESULTS " +
            json.dumps(CASES if values is None else values) + "\n")


def llvm(values=None, case_count=None, fixture_blob=FIXTURE_BLOB):
    v = CASES if values is None else values
    n = len(v) if case_count is None else case_count
    return ("LLVM_GUEST_SHARED_HANDLER_PASS cases=" + str(n) +
            " fixture_blob=" + fixture_blob + "\n" +
            json.dumps({"backend": "llvm", "observed": v}, sort_keys=True) + "\n")


class HttpGuestLogAdapterTest(unittest.TestCase):
    def test_graal_raw_output(self):
        self.assertEqual(CASES, normalize("graal", graal(), FIXTURE_BLOB)["observed"])

    def test_llvm_raw_output(self):
        self.assertEqual(CASES, normalize("llvm", llvm(), FIXTURE_BLOB)["observed"])

    def test_wrong_but_identical_results_rejected(self):
        wrong = dict(CASES, **{"counter_response(1)": 44})
        for backend, log in (("graal", graal(wrong)), ("llvm", llvm(wrong))):
            with self.subTest(backend=backend):
                with self.assertRaisesRegex(LogAdapterError, "expected 43"):
                    normalize(backend, log, FIXTURE_BLOB)

    def test_duplicate_markers_rejected(self):
        for backend, log in (("graal", graal() + graal()),
                             ("llvm", llvm() + llvm())):
            with self.subTest(backend=backend):
                with self.assertRaisesRegex(LogAdapterError, "exactly one"):
                    normalize(backend, log, FIXTURE_BLOB)

    def test_missing_real_output_rejected(self):
        for backend in ("graal", "llvm"):
            with self.subTest(backend=backend):
                with self.assertRaises(LogAdapterError):
                    normalize(backend, "build success, no guest executed", FIXTURE_BLOB)

    def test_llvm_count_and_blob_checks(self):
        with self.assertRaisesRegex(LogAdapterError, "count mismatch"):
            normalize("llvm", llvm(case_count=len(CASES) + 1), FIXTURE_BLOB)
        with self.assertRaisesRegex(LogAdapterError, "fixture identity"):
            normalize("llvm", llvm(fixture_blob="0" * 40), FIXTURE_BLOB)

    def test_incorrect_requested_blob_rejected(self):
        with self.assertRaisesRegex(LogAdapterError, "canonical source"):
            normalize("graal", graal(), "0" * 40)

    def test_missing_probes_rejected(self):
        fewer = dict(CASES)
        del fewer["counter_response(-1)"]
        with self.assertRaisesRegex(LogAdapterError, "missing required"):
            normalize("graal", graal(fewer), FIXTURE_BLOB)

    def test_duplicate_json_keys_rejected(self):
        bad = graal().replace('"main()": 43', '"main()": 42, "main()": 43')
        with self.assertRaisesRegex(LogAdapterError, "duplicate JSON key"):
            normalize("graal", bad, FIXTURE_BLOB)

    def test_llvm_machine_record_wrong_backend_rejected(self):
        bad = llvm().replace('"backend": "llvm"', '"backend": "graal"')
        with self.assertRaisesRegex(LogAdapterError, "backend identity"):
            normalize("llvm", bad, FIXTURE_BLOB)

    def test_large_log_rejected(self):
        with self.assertRaisesRegex(LogAdapterError, "oversized"):
            normalize("graal", "x" * (2 * 1024 * 1024 + 1), FIXTURE_BLOB)

    def test_wrong_marker_backend_rejected(self):
        with self.assertRaises(LogAdapterError):
            normalize("graal", llvm(), FIXTURE_BLOB)
        with self.assertRaises(LogAdapterError):
            normalize("llvm", graal(), FIXTURE_BLOB)


if __name__ == "__main__":
    unittest.main()
