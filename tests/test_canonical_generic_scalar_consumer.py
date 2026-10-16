#!/usr/bin/env python3
"""Fail-closed canonical LLVM contract consumer smoke tests (offline)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/verify_canonical_generic_scalar.py"
spec = importlib.util.spec_from_file_location("pinned_generic_gate_llvm", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class CanonicalGenericConsumerTests(unittest.TestCase):
    def test_pin_cannot_silently_change(self):
        self.assertEqual(gate.COMMON_ROOT_TREE, "858c37d48a4c3a886260b26320bb0dd931b228ab")
        self.assertEqual(gate.COMMON_CONTRACT_TREE, "fd5301ebd354049917e2ecaf1da920b661d6d829")
        self.assertEqual(len(gate.COMMON_SUITE_DIGEST), 64)

    def test_missing_canonical_checkout_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                gate.prove_canonical_source(Path(tmp))


if __name__ == "__main__":
    unittest.main()
