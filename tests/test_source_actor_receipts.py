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
        for value in ("4junk", "4.0", "+4", ""):
            with self.subTest(value=value), self.assertRaises(AssertionError):
                parse(self.good.replace("processed=4", "processed=" + value))

    def test_wrong_marker_is_denied(self):
        with self.assertRaises(AssertionError):
            parse("OTHER_MARKER " + self.good)


if __name__ == "__main__":
    unittest.main()
