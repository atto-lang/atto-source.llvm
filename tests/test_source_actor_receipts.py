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
            "released=1 ready=1 accepted=4 processed=4 abandoned=0 last=52"
        )

    def test_exact_native_receipt(self):
        self.assertEqual(
            {
                "constructor_on_worker": 1,
                "released": 1,
                "ready": 1,
                "accepted": 4,
                "processed": 4,
                "abandoned": 0,
                "last": 52,
            },
            parse(self.good),
        )

    def test_failed_startup_readiness_is_a_valid_receipt_value(self):
        self.assertEqual(0, parse(self.good.replace("ready=1", "ready=0"))["ready"])

    def test_invalid_readiness_is_denied(self):
        with self.assertRaises(AssertionError):
            parse(self.good.replace("ready=1", "ready=2"))

    def test_negative_result_is_supported(self):
        self.assertEqual(-172, parse(self.good.replace("last=52", "last=-172"))["last"])

    def test_delivery_count_prefix_does_not_pass(self):
        self.assertNotEqual(
            4, parse(self.good.replace("processed=4", "processed=40"))["processed"])

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
            parse(self.good.replace(" abandoned=0", ""))

    def test_non_decimal_count_is_denied(self):
        for value in ("4junk", "4.0", "+4", "", "４", "٤"):
            with self.subTest(value=value), self.assertRaises(AssertionError):
                parse(self.good.replace("processed=4", "processed=" + value))

    def test_wrong_marker_is_denied(self):
        with self.assertRaises(AssertionError):
            parse("OTHER_MARKER " + self.good)


    def test_c_integer_abi_boundaries(self):
        decimal = source_actor_e2e.parse_native_decimal
        self.assertEqual((1 << 64) - 1, decimal(str((1 << 64) - 1)))
        self.assertEqual((1 << 63) - 1, decimal(str((1 << 63) - 1), signed=True))
        self.assertEqual(-(1 << 63), decimal(str(-(1 << 63)), signed=True))

    def test_c_integer_overflow_is_denied(self):
        for old, new in (
            ("accepted=4", "accepted=18446744073709551616"),
            ("last=52", "last=9223372036854775808"),
            ("last=52", "last=-9223372036854775809"),
        ):
            with self.subTest(new=new), self.assertRaises(AssertionError):
                parse(self.good.replace(old, new))

    def test_noncanonical_leading_zero_negative_zero_and_sign_are_denied(self):
        for old, new in (
            ("processed=4", "processed=04"),
            ("ready=1", "ready=01"),
            ("last=52", "last=052"),
            ("last=52", "last=-0"),
            ("accepted=4", "accepted=+4"),
            ("last=52", "last=+52"),
        ):
            with self.subTest(new=new), self.assertRaises(AssertionError):
                parse(self.good.replace(old, new))

    def test_exact_native_abi_field_order_and_framing(self):
        for altered in (
            self.good.replace("ready=1 accepted=4", "accepted=4 ready=1"),
            self.good.replace(" released=1", "  released=1"),
            self.good.replace(" released=1", "\treleased=1"),
            self.good.replace(" released=1", "\u2003released=1"),
            self.good + " ",
            self.good.replace("last=52", "last=52 extra=1"),
        ):
            with self.subTest(altered=altered), self.assertRaises(AssertionError):
                parse(altered)


if __name__ == "__main__":
    unittest.main()
