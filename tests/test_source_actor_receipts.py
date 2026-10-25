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


class ActorHeapPrefixOracleTest(unittest.TestCase):
    def test_independent_heap_prefixes(self):
        # Hand-calculated from total = 2 * (total + message), with
        # interleaved actor inputs 1..4 and 5..8. Include partial STOPs.
        for actor, expected in enumerate(((0, 2, 8, 22, 52),
                                          (0, 10, 32, 78, 172))):
            for processed, last in enumerate(expected):
                with self.subTest(actor=actor, processed=processed):
                    self.assertEqual(last, source_actor_e2e.expected_prefix_last(
                        "source-actor-two-owned-heaps.ores", actor, processed))

    def test_existing_fixture_prefixes_are_preserved(self):
        cases = (
            ("source-actor-scoped-owned-i64.ores", 0, 8, 1004),
            ("source-actor-fifo-ten.ores", 0, 10, 2853116705),
            ("source-actor-interleaved-stop.ores", 0, 1, 1),
            ("source-actor-interleaved-stop.ores", 1, 2, 7),
            ("source-actor-send.ores", 0, 2, 3),
        )
        for name, actor, processed, last in cases:
            with self.subTest(name=name, actor=actor):
                self.assertEqual(last, source_actor_e2e.expected_prefix_last(
                    name, actor, processed))


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


if __name__ == "__main__":
    unittest.main()
