#!/usr/bin/env python3
"""Pure admission-policy tests; no compiler, wabt toolchain or guest execution.

The protection is *before* a tool is called: if an arbitrary test macro can
redefine WASM_RT_MEMCHECK_BOUNDS_CHECK, a build falsely labeled as bounds-checked
could access a sibling actor's linear memory. This suite is an independent gate
for that policy, not a replacement for compiled-guest proof.
"""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "tools" / "tenant-vm" / "lower_guest.py"
spec = importlib.util.spec_from_file_location("tenant_vm_lowering_policy_under_test", SOURCE)
lowerer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lowerer)


class LoweringPolicyTests(unittest.TestCase):
    def test_normal_bounds_build_is_default_with_no_test_injection(self):
        self.assertEqual(lowerer.checked_test_defines([]), [])

    def test_existing_fault_injection_knobs_are_exactly_allowlisted(self):
        allowed = [
            "ORES_VM_TEST_EAGER_SEND=1",
            "ORES_VM_TEST_COMMIT_FAULT_ON=77",
            "ORES_VM_TEST_UNSAFE_COMMIT=1",
        ]
        self.assertEqual(lowerer.checked_test_defines(allowed), allowed)

    def test_security_sensitive_macros_are_never_user_overridable(self):
        attacks = [
            "WASM_RT_MEMCHECK_BOUNDS_CHECK=0",
            "WASM_RT_MEMCHECK_GUARD_PAGES=1",
            "WASM_RT_SKIP_SIGNAL_RECOVERY=1",
            "ORES_VM_MEMORY_BYTES=1",
            "ORES_VM_SCRATCH_BYTES=1",
            "ORES_VM_SOURCE_ACTOR=0",
            "ORES_VM_ACTOR_FLAGS=0",
            "ORES_VM_TEST_NOT_ALLOWLISTED=1",
            "ORES_VM_TEST_EAGER_SEND=0",
            "ORES_VM_TEST_EAGER_SEND=2",
            "ORES_VM_TEST_COMMIT_FAULT_ON=2147483648",
            "ORES_VM_TEST_COMMIT_FAULT_ON=" + "9" * 5000,
            "ORES_VM_TEST_COMMIT_FAULT_ON=1+1",
            "ORES_VM_TEST_EAGER_SEND=1 -DWASM_RT_MEMCHECK_BOUNDS_CHECK=0",
            "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=0",
        ]
        for attack in attacks:
            with self.subTest(attack=attack), self.assertRaises(lowerer.Refused):
                lowerer.checked_test_defines([attack])

    def test_duplicates_and_excessive_defines_are_refused(self):
        with self.assertRaises(lowerer.Refused):
            lowerer.checked_test_defines(["ORES_VM_TEST_EAGER_SEND=1"] * 2)
        with self.assertRaises(lowerer.Refused):
            lowerer.checked_test_defines(["ORES_VM_TEST_EAGER_SEND=1"] * 4)

    def test_guard_mode_without_explicit_test_only_fails_before_tool_invocation(self):
        args = SimpleNamespace(mode="guard", test_only=False, test_define=[])
        with self.assertRaisesRegex(lowerer.Refused, "guard-page-only"):
            lowerer.lower(args)  # No 'out' attribute: must reject before accessing it.

    def test_test_macros_without_explicit_test_only_fail_before_tool_invocation(self):
        args = SimpleNamespace(mode="bounds", test_only=False, test_define=["ORES_VM_TEST_EAGER_SEND=1"])
        with self.assertRaisesRegex(lowerer.Refused, "test preprocessor"):
            lowerer.lower(args)

    def test_unknown_macro_with_test_consent_still_fails_before_tool_invocation(self):
        args = SimpleNamespace(mode="bounds", test_only=True,
                               test_define=["WASM_RT_MEMCHECK_BOUNDS_CHECK=0"])
        with self.assertRaises(lowerer.Refused):
            lowerer.lower(args)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LoweringPolicyTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print("LOWERING_POLICY_PASS" if result.wasSuccessful() else "LOWERING_POLICY_FAIL")
    sys.exit(0 if result.wasSuccessful() else 1)
