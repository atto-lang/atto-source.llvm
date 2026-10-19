#!/usr/bin/env python3
"""Source-free fail-closed tests for the test-only actor field ABI gate.

This does not compile Oreslang, sign artifacts, load native code, or certify
a runtime. It ensures that a malformed/mismatched test fixture cannot cause
the AOT/PIC harness to assume ABI compatibility via hard-coded metadata.
"""
from pathlib import Path
import unittest

from source_actor_pic_hot_reload import (
    EXPECTED_FIELDS, source_actor_layout, validate_source_fixture_pair
)

FIXTURES = Path(__file__).parent / "fixtures"


class SourceActorPicLayoutSelfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v1 = (FIXTURES / "actor-hot-reload-aot-v1.ores").read_text("utf-8")
        cls.v2 = (FIXTURES / "actor-hot-reload-aot-v2.ores").read_text("utf-8")

    def test_actual_sources_identical_layout(self):
        self.assertEqual(source_actor_layout(self.v1), EXPECTED_FIELDS)
        self.assertEqual(source_actor_layout(self.v2), EXPECTED_FIELDS)
        validate_source_fixture_pair(self.v1, self.v2)

    def test_rename_denied(self):
        changed = self.v2.replace("let total: int", "let replacement: int", 1)
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_immutability_change_denied(self):
        changed = self.v2.replace("let total: int", "val total: int", 1)
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_type_change_denied(self):
        changed = self.v2.replace("let total: int", "let total: string", 1)
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_field_reorder_denied(self):
        changed = self.v2.replace(
            "val delta: int = 2;\n  let total: int = 0;",
            "let total: int = 0;\n  val delta: int = 2;", 1
        )
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_extra_field_denied(self):
        changed = self.v2.replace(
            "  pub fnc on_message(",
            "  let injected: int = 1;\n  pub fnc on_message(", 1
        )
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_missing_handler_denied(self):
        changed = self.v2.replace("pub fnc on_message(", "pub fnc other(")
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)

    def test_duplicate_handler_denied(self):
        changed = self.v2.replace(
            "\n}", "\n  pub fnc on_message(int z): int { return z; }\n}",
            1
        )
        self.assertNotEqual(changed, self.v2)
        with self.assertRaises(ValueError):
            source_actor_layout(changed)


if __name__ == "__main__":
    unittest.main(verbosity=2)
