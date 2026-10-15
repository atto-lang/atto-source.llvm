#!/usr/bin/env python3
"""Adversarial tests for the shared native generic contract admission boundary."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from native_scalar_generic_source_execution import load_shared_conformance

ROOT = Path(__file__).resolve().parents[1]
REAL = ROOT / "contracts/v1/generics-stage1/cases.json"
BASE = {
    "schema_version": "1.0.0",
    "scope": "native-scalar-i64-only",
    "cases": [
        {"id": "generics.scalar.good", "kind": "native_exit",
         "source": "pub fnc main(): int { return 42; }\n", "expected_exit": 42},
        {"id": "generics.scalar.bad", "kind": "diagnostic",
         "source": "not valid\n", "expected_substring": "syntax"}
    ]
}

class ContractAdmissionTests(unittest.TestCase):
    def validate(self, payload):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cases.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            return load_shared_conformance(path)

    def test_repository_contract(self):
        valid, invalid = load_shared_conformance(REAL)
        self.assertEqual((len(valid), len(invalid)), (3, 3))

    def test_accept_two_kinds(self):
        valid, invalid = self.validate(BASE)
        self.assertEqual(valid["generics.scalar.good"][1], 42)
        self.assertEqual(invalid["generics.scalar.bad"][1], "syntax")

    def test_reject_unsafe_names_and_collisions(self):
        for bad in ("../escape", "generics.scalar.x/../../escape",
                    "generics.scalar.x\nbad", "generics.scalar..bad",
                    "explicit-scalar", "generics.scalar.UPPER"):
            with self.subTest(bad=bad):
                payload = copy.deepcopy(BASE)
                payload["cases"][0]["id"] = bad
                with self.assertRaises(AssertionError):
                    self.validate(payload)
        payload = copy.deepcopy(BASE)
        payload["cases"][1]["id"] = payload["cases"][0]["id"]
        with self.assertRaises(AssertionError):
            self.validate(payload)

    def test_reject_invalid_schema_scope_and_inventory(self):
        for key, value in (("schema_version", "2.0.0"),
                           ("scope", "all-backends"), ("cases", []),
                           ("cases", {}), ("cases", [None])):
            with self.subTest(key=key):
                payload = copy.deepcopy(BASE)
                payload[key] = value
                with self.assertRaises(AssertionError):
                    self.validate(payload)

    def test_reject_bad_values(self):
        mutations = [
            {"expected_exit": True}, {"expected_exit": -1},
            {"expected_exit": 256}, {"expected_exit": "42"},
            {"source": ""}, {"source": "a" * 65_537},
            {"source": "a\x00b"}, {"unexpected": 1}
        ]
        for fields in mutations:
            with self.subTest(fields=list(fields)):
                payload = copy.deepcopy(BASE)
                payload["cases"][0].update(fields)
                with self.assertRaises(AssertionError):
                    self.validate(payload)
        for fields in ({"expected_substring": ""}, {"expected_substring": False},
                       {"expected_substring": "a" * 257}, {"expected_exit": 42}):
            with self.subTest(fields=list(fields)):
                payload = copy.deepcopy(BASE)
                payload["cases"][1].update(fields)
                with self.assertRaises(AssertionError):
                    self.validate(payload)

    def test_reject_too_large_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cases.json"
            path.write_text(" " * (1_048_576 + 1), encoding="utf-8")
            with self.assertRaises(AssertionError):
                load_shared_conformance(path)

if __name__ == "__main__":
    unittest.main()
