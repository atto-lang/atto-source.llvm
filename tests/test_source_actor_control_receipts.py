#!/usr/bin/env python3
"""Synthetic negative controls; not compiled Oreslang actor execution."""
import importlib.util
import pathlib
import unittest

harness = pathlib.Path(__file__).with_name("source_actor_e2e.py")
spec = importlib.util.spec_from_file_location("source_actor_e2e", harness)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
parse = module.parse_native_control_receipt

VM_MARKER = "SOURCE_ACTOR_VM_ROUTING_VERIFIED"
READY_MARKER = "SOURCE_AWAIT_READY_TRACE_PASS"
DONE_MARKER = "SOURCE_AWAIT_DONE_TRACE_PASS"
VM = "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0 root_slot=63"
READY = ("SOURCE_AWAIT_READY_TRACE_PASS outcome=2 execution_id_persisted=1 "
         "fresh_resume=1 root_slot=63 source_token=456")
DONE = ("SOURCE_AWAIT_DONE_TRACE_PASS terminal_ack=1 execution_id_persisted=1 "
        "fresh_resume=1 root_slot=63 source_token=456")
VM_EXPECTED = {"domains": 4, "main": 1, "cold_carriers": 0, "root_slot": 63}
READY_EXPECTED = {"outcome": 2, "execution_id_persisted": 1,
                  "fresh_resume": 1, "root_slot": 63}
DONE_EXPECTED = {"terminal_ack": 1, "execution_id_persisted": 1,
                 "fresh_resume": 1, "root_slot": 63}


class NativeControlReceiptTest(unittest.TestCase):
    def test_valid_vm_routing(self):
        self.assertEqual(VM_EXPECTED, parse(VM, VM_MARKER, VM_EXPECTED))

    def test_valid_ready_outcome_and_dynamic_token(self):
        self.assertEqual(456, parse(
            READY, READY_MARKER, READY_EXPECTED, ("source_token",))["source_token"])

    def test_valid_done_ack_and_dynamic_token(self):
        self.assertEqual(456, parse(
            DONE, DONE_MARKER, DONE_EXPECTED, ("source_token",))["source_token"])

    def test_wrong_ready_outcome_and_numeric_prefix_denied(self):
        for bad in ("outcome=20", "outcome=3", "outcome=0", "outcome=02"):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                parse(READY.replace("outcome=2", bad), READY_MARKER,
                      READY_EXPECTED, ("source_token",))

    def test_wrong_done_terminal_ack_denied(self):
        with self.assertRaises(AssertionError):
            parse(DONE.replace("terminal_ack=1", "terminal_ack=0"),
                  DONE_MARKER, DONE_EXPECTED, ("source_token",))

    def test_wrong_vm_values_denied(self):
        for wrong in ("domains=40", "main=10", "cold_carriers=1", "root_slot=630"):
            good = wrong.split("=")[0] + "="
            with self.subTest(wrong=wrong), self.assertRaises(AssertionError):
                parse(VM.replace(good + VM.split(good, 1)[1].split()[0], wrong),
                      VM_MARKER, VM_EXPECTED)

    def test_duplicate_extra_and_missing_fields_denied(self):
        for bad in (VM + " main=1", VM + " attacker=0",
                    VM.replace(" cold_carriers=0", "")):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                parse(bad, VM_MARKER, VM_EXPECTED)

    def test_field_reordering_and_whitespace_denied(self):
        for bad in (VM.replace("domains=4 main=1", "main=1 domains=4"),
                    VM.replace("main=1", "  main=1"),
                    VM.replace("main=1", "\tmain=1"),
                    VM + " "):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                parse(bad, VM_MARKER, VM_EXPECTED)

    def test_unicode_numbers_and_separators_denied(self):
        for bad in (READY.replace("source_token=456", "source_token=٤٥٦"),
                    READY.replace("source_token=456", "source_token=４５６"),
                    READY.replace(" source_token=", "\u2003source_token=")):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                parse(bad, READY_MARKER, READY_EXPECTED, ("source_token",))

    def test_overflow_or_noncanonical_source_token_denied(self):
        for raw in ("18446744073709551616", "0456", "-1", "+456", "4junk", ""):
            with self.subTest(raw=raw), self.assertRaises(AssertionError):
                parse(READY.replace("source_token=456", "source_token=" + raw),
                      READY_MARKER, READY_EXPECTED, ("source_token",))

    def test_other_control_marker_or_extra_prefix_denied(self):
        for bad in (READY.replace(READY_MARKER, DONE_MARKER),
                    "prefix " + READY):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                parse(bad, READY_MARKER, READY_EXPECTED, ("source_token",))


if __name__ == "__main__":
    unittest.main()
