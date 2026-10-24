#!/usr/bin/env python3
"""Source-free hostile-receipt parser tests; not native actor execution."""
import importlib.util
import pathlib
import unittest

harness_path = pathlib.Path(__file__).with_name("source_actor_e2e.py")
spec = importlib.util.spec_from_file_location("source_actor_e2e", harness_path)
assert spec and spec.loader
source_actor_e2e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source_actor_e2e)
parse = source_actor_e2e.parse_actor_receipt


class ActorReceiptParserTest(unittest.TestCase):
    def setUp(self):
        self.good = (
            "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 "
            "released=1 processed=4 last=52"
        )

    def test_exact_native_receipt(self):
        self.assertEqual(
            {"constructor_on_worker": 1, "released": 1, "processed": 4, "last": 52},
            parse(self.good),
        )

    def test_negative_result_is_supported(self):
        self.assertEqual(-172, parse(self.good.replace("last=52", "last=-172"))["last"])

    def test_delivery_count_prefix_does_not_pass(self):
        self.assertNotEqual(4, parse(self.good.replace("processed=4", "processed=40"))["processed"])

    def test_release_flag_prefix_is_denied(self):
        with self.assertRaises(AssertionError):
            parse(self.good.replace("released=1", "released=10"))

    def test_duplicate_field_is_denied(self):
        with self.assertRaises(AssertionError):
            parse(self.good + " processed=4")

    def test_unknown_field_is_denied(self):
        with self.assertRaises(AssertionError):
            parse(self.good + " injected=1")

    def test_missing_field_is_denied(self):
        with self.assertRaises(AssertionError):
            parse(self.good.replace(" processed=4", ""))

    def test_non_decimal_count_is_denied(self):
        for value in ("4junk", "4.0", "+4", "", "４", "٤"):
            with self.subTest(value=value), self.assertRaises(AssertionError):
                parse(self.good.replace("processed=4", "processed=" + value))

    def test_wrong_marker_is_denied(self):
        with self.assertRaises(AssertionError):
            parse("OTHER_MARKER " + self.good)



class NativeControlReceiptParserTest(unittest.TestCase):
    def setUp(self):
        self.parse = source_actor_e2e.parse_native_control_receipt
        self.vm = (
            "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 "
            "cold_carriers=0 root_slot=63"
        )
        self.vm_expected = {
            "domains": 4, "main": 1, "cold_carriers": 0, "root_slot": 63
        }
        self.trace = (
            "SOURCE_AWAIT_READY_TRACE_PASS execution_id_persisted=1 "
            "fresh_resume=1 root_slot=63 source_token=1234"
        )
        self.trace_expected = {
            "execution_id_persisted": 1, "fresh_resume": 1, "root_slot": 63
        }

    def test_exact_vm_routing_receipt(self):
        self.assertEqual(self.vm_expected, self.parse(
            self.vm, "SOURCE_ACTOR_VM_ROUTING_VERIFIED", self.vm_expected))

    def test_exact_continuation_receipt_with_dynamic_source_token(self):
        result = self.parse(
            self.trace, "SOURCE_AWAIT_READY_TRACE_PASS",
            self.trace_expected, dynamic=("source_token",))
        self.assertEqual(1234, result["source_token"])

    def test_prefix_and_wrong_root_values_are_denied(self):
        for bad in (self.vm.replace("domains=4", "domains=40"),
                    self.vm.replace("root_slot=63", "root_slot=630"),
                    self.vm.replace("main=1", "main=10")):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                self.parse(bad, "SOURCE_ACTOR_VM_ROUTING_VERIFIED", self.vm_expected)

    def test_forged_duplicate_unknown_and_missing_control_fields_are_denied(self):
        for bad in (self.vm + " main=1", self.vm + " fake=1",
                    self.vm.replace(" root_slot=63", "")):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                self.parse(bad, "SOURCE_ACTOR_VM_ROUTING_VERIFIED", self.vm_expected)

    def test_malformed_trace_source_tokens_are_denied(self):
        for bad_token in ("", "+1", "-1", "1tail", "1.0", "１２", "١"):
            with self.subTest(token=bad_token), self.assertRaises(AssertionError):
                self.parse(
                    self.trace.replace("source_token=1234", "source_token=" + bad_token),
                    "SOURCE_AWAIT_READY_TRACE_PASS",
                    self.trace_expected, dynamic=("source_token",))

    def test_unexpected_continuation_marker_is_denied(self):
        with self.assertRaises(AssertionError):
            self.parse(self.trace, "SOURCE_ACTOR_VM_ROUTING_VERIFIED",
                       self.trace_expected, dynamic=("source_token",))



class CanonicalNativeReceiptTest(unittest.TestCase):
    def setUp(self):
        self.actor = (
            "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 "
            "released=1 processed=4 last=52"
        )
        self.vm = "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0 root_slot=63"
        self.vm_expected = {"domains": 4, "main": 1, "cold_carriers": 0, "root_slot": 63}
        self.trace = (
            "SOURCE_AWAIT_READY_TRACE_PASS execution_id_persisted=1 "
            "fresh_resume=1 root_slot=63 source_token=1234"
        )
        self.trace_expected = {
            "execution_id_persisted": 1, "fresh_resume": 1, "root_slot": 63
        }

    def test_c_int64_and_uint64_boundary_values(self):
        parse = source_actor_e2e.parse_native_decimal
        self.assertEqual((1 << 64) - 1, parse(str((1 << 64) - 1)))
        self.assertEqual(-(1 << 63), parse(str(-(1 << 63)), signed=True))
        self.assertEqual((1 << 63) - 1, parse(str((1 << 63) - 1), signed=True))

    def test_noncanonical_or_out_of_range_actor_numbers_are_denied(self):
        for old, new in (
            ("processed=4", "processed=04"),
            ("last=52", "last=052"),
            ("last=52", "last=-0"),
            ("processed=4", "processed=18446744073709551616"),
            ("last=52", "last=9223372036854775808"),
            ("last=52", "last=-9223372036854775809"),
        ):
            with self.subTest(new=new), self.assertRaises(AssertionError):
                source_actor_e2e.parse_actor_receipt(self.actor.replace(old, new))

    def test_noncanonical_vm_and_continuation_numbers_are_denied(self):
        for old, new in (
            ("domains=4", "domains=04"),
            ("root_slot=63", "root_slot=063"),
            ("root_slot=63", "root_slot=18446744073709551616"),
        ):
            with self.subTest(new=new), self.assertRaises(AssertionError):
                source_actor_e2e.parse_native_control_receipt(
                    self.vm.replace(old, new),
                    "SOURCE_ACTOR_VM_ROUTING_VERIFIED", self.vm_expected)
        for new in ("source_token=01234", "source_token=18446744073709551616"):
            with self.subTest(new=new), self.assertRaises(AssertionError):
                source_actor_e2e.parse_native_control_receipt(
                    self.trace.replace("source_token=1234", new),
                    "SOURCE_AWAIT_READY_TRACE_PASS", self.trace_expected,
                    dynamic=("source_token",))

    def test_noncanonical_framing_and_field_order_are_denied(self):
        for bad in (
            self.actor.replace(" processed=4", "\tprocessed=4"),
            self.actor.replace("processed=4", "processed=4  "),
            self.actor.replace("processed=4 last=52", "last=52 processed=4"),
            self.actor.replace(" processed=4", "\u2003processed=4"),
        ):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                source_actor_e2e.parse_actor_receipt(bad)
        for bad in (
            self.vm.replace(" main=1", "  main=1"),
            self.vm.replace("domains=4 main=1", "main=1 domains=4"),
            self.vm.replace(" main=1", "\u2003main=1"),
        ):
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                source_actor_e2e.parse_native_control_receipt(
                    bad, "SOURCE_ACTOR_VM_ROUTING_VERIFIED", self.vm_expected)


if __name__ == "__main__":
    unittest.main()
