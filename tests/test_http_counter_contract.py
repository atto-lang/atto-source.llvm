import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from http_counter_contract import (
    CounterContractError, verify_counter_observations,
)

CASES = {
    "main()": 43,
    "counter_response(-1)": -43,
    "counter_response(0)": 0,
    "counter_response(1)": 43,
    "counter_response(2)": 86,
    "counter_response(7)": 301,
}


class HttpCounterContractTest(unittest.TestCase):
    def test_canonical_semantics(self):
        self.assertEqual(6, verify_counter_observations(CASES))

    def test_full_131_matrix_required_for_parity(self):
        full = dict(CASES)
        full.update({f"counter_response({i})": 43 * i for i in range(1, 129)})
        self.assertEqual(131, verify_counter_observations(full, require_full_matrix=True))
        with self.assertRaisesRegex(CounterContractError, "missing"):
            verify_counter_observations(CASES, require_full_matrix=True)
        missing = dict(full)
        del missing["counter_response(128)"]
        with self.assertRaisesRegex(CounterContractError, "missing"):
            verify_counter_observations(missing, require_full_matrix=True)
        extra = dict(full)
        extra["counter_response(129)"] = 129 * 43
        with self.assertRaisesRegex(CounterContractError, "unexpected full-matrix"):
            verify_counter_observations(extra, require_full_matrix=True)
        bad = dict(full)
        bad["counter_response(127)"] = -1
        with self.assertRaisesRegex(CounterContractError, "expected"):
            verify_counter_observations(bad, require_full_matrix=True)

    def test_identically_wrong_backends_cannot_pass(self):
        bad = dict(CASES, **{"counter_response(1)": 44})
        with self.assertRaisesRegex(CounterContractError, "expected 43"):
            verify_counter_observations(bad)

    def test_no_missing_required_probes(self):
        for case in ("main()", "counter_response(0)", "counter_response(-1)"):
            with self.subTest(case=case):
                bad = dict(CASES)
                del bad[case]
                with self.assertRaisesRegex(CounterContractError, "missing"):
                    verify_counter_observations(bad)

    def test_reject_unrecognized_cases(self):
        for case in ("arbitrary()", "counter_response(01)", "counter_response(+1)",
                     "counter_response(1) trailing", "counter_response(-0)"):
            with self.subTest(case=case):
                with self.assertRaisesRegex(CounterContractError, "unknown case"):
                    verify_counter_observations(dict(CASES, **{case: 1}))

    def test_reject_overflow_inputs(self):
        for argument in (2**63, -(2**63) - 1):
            with self.subTest(argument=argument):
                with self.assertRaisesRegex(CounterContractError, "outside signed i64"):
                    verify_counter_observations(dict(CASES, **{
                        f"counter_response({argument})": 0
                    }))
        with self.assertRaisesRegex(CounterContractError, "arithmetic overflow"):
            argument = (2**63 // 43) + 1
            verify_counter_observations(dict(CASES, **{
                f"counter_response({argument})": 0
            }))

    def test_largest_safe_positive_probe(self):
        argument = 2**63 // 43
        expected = argument * 43
        self.assertLessEqual(expected, 2**63 - 1)
        self.assertEqual(7, verify_counter_observations(dict(CASES, **{
            f"counter_response({argument})": expected
        })))

    def test_reject_boolean_outputs(self):
        with self.assertRaises(CounterContractError):
            verify_counter_observations(dict(CASES, **{"main()": True}))


if __name__ == "__main__":
    unittest.main()
