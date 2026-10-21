"""Host-only regression checks for native FSM gate; not guest compilation."""
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "tests/native_fsm_scalar_guest.py"
spec = importlib.util.spec_from_file_location("native_fsm_scalar_guest", MODULE)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class FSMNativeHarnessContract(unittest.TestCase):
    def test_enforcement_survives_python_optimized_mode(self):
        with self.assertRaises(AssertionError):
            gate.require(False, "must fail even under python -O")

    def test_packed_tuple_aliases_are_rejected_by_oracle(self):
        for state, event in (
            (-1, 10), (0, 11), (0, 12), (0, 23),
            (2, -9), (2, -8), (3, -7), (99, -978),
        ):
            for ticks in (0, 2, 3):
                with self.subTest(state=state, event=event, ticks=ticks):
                    self.assertEqual(gate.expected(state, event, ticks), -1)

    def test_trace_oracle_adapter_is_explicitly_bounded_and_fail_closed(self):
        class FakeOracle:
            LIMIT = (1 << 32) - 1

            @staticmethod
            def snap(state, version, ticks):
                return {"state": state, "version": version, "ticks": ticks}

            @staticmethod
            def step(before, event):
                if before["state"] == 0 and event == 0 and before["ticks"] <= 3:
                    return True, "none", {"state": 1, "version": 1,
                                          "ticks": 0}, [1]
                return False, "unhandled", before, []

        model = FakeOracle()
        self.assertEqual(gate.oracle_expected(model, 0, 0, 2), 1)
        self.assertEqual(gate.oracle_expected(model, 1, 99, 1), -1)
        self.assertIsNone(gate.oracle_expected(model, -1, 0, 1))
        self.assertIsNone(gate.oracle_expected(model, 0, -1, 1))
        self.assertIsNone(gate.oracle_expected(model, 0, 0, -1))
        self.assertIsNone(gate.oracle_expected(model, 0, 0, 1 << 32))
        with self.assertRaises(AssertionError):
            gate.load_trace_oracle(ROOT / "tests/missing-contract-does-not-exist.py")

    def test_guest_fixture_does_not_pack_untrusted_coordinates(self):
        source = (ROOT / "tests/fixtures/fsm-scalar-reducer.ores").read_text(encoding="utf-8")
        self.assertNotIn("state * 10 + event", source)
        self.assertIn("if (state eq 0)", source)
        self.assertIn("if (event eq 0)", source)


if __name__ == "__main__":
    unittest.main()
