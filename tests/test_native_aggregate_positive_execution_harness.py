#!/usr/bin/env python3
"""Source-free adversarial tests for native exact-struct guest admission.

These are NOT .ores compiler executions or evidence of LLVM guest parity.
"""
import subprocess
import unittest

from native_aggregate_positive_execution import (
    POSITIVE, NEGATIVE, verify_guest_process, verify_negative_rejection,
)


class NativeAggregatePositiveHarnessTest(unittest.TestCase):
    diagnostic = "AGGREGATE_FIELD_TYPE_UNVERIFIED"

    def check_result(self, rc=1, out=b"", err=b"AGGREGATE_FIELD_TYPE_UNVERIFIED"):
        result = subprocess.CompletedProcess(["synthetic-compiler"], rc, out, err)
        verify_negative_rejection(result, self.diagnostic)

    def test_exact_diagnostic_accepted(self):
        self.check_result()

    def test_registered_fixture_exits_are_distinct(self):
        self.assertEqual(len(POSITIVE), 4)
        self.assertEqual(len(NEGATIVE), 4)
        self.assertEqual(set(status for _, _, _, status in POSITIVE), {7, 19, 42, 73})

    def test_real_guest_status_accepted(self):
        observed = subprocess.CompletedProcess(["guest"], 19, b"", b"")
        verify_guest_process("named-source-order", observed, 19)

    def test_constant_42_mock_rejected_for_19_case(self):
        observed = subprocess.CompletedProcess(["fake-constant-42-compiler"], 42, b"", b"")
        with self.assertRaisesRegex(RuntimeError, "expected OS exit 19"):
            verify_guest_process("named-source-order", observed, 19)

    def test_guest_stdout_rejected(self):
        observed = subprocess.CompletedProcess(["guest"], 19, b"unwanted", b"")
        with self.assertRaisesRegex(RuntimeError, "expected OS exit 19"):
            verify_guest_process("named-source-order", observed, 19)

    def test_guest_stderr_rejected(self):
        observed = subprocess.CompletedProcess(["guest"], 19, b"", b"unwanted")
        with self.assertRaisesRegex(RuntimeError, "expected OS exit 19"):
            verify_guest_process("named-source-order", observed, 19)

    def test_guest_signal_rejected(self):
        observed = subprocess.CompletedProcess(["guest"], -9, b"", b"")
        with self.assertRaisesRegex(RuntimeError, "expected OS exit 19"):
            verify_guest_process("named-source-order", observed, 19)

    def test_prefixed_diagnostic_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "complete identifier"):
            self.check_result(err=b"PREFIX_AGGREGATE_FIELD_TYPE_UNVERIFIED")

    def test_suffixed_diagnostic_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "complete identifier"):
            self.check_result(err=b"AGGREGATE_FIELD_TYPE_UNVERIFIED_FAKE")

    def test_different_diagnostic_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "complete identifier"):
            self.check_result(err=b"AGGREGATE_RETURN_TYPE_MISMATCH")

    def test_ir_on_stdout_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "emitted output"):
            self.check_result(out=b"; ModuleID = 'illicit'")

    def test_ir_on_stderr_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "leaked LLVM IR"):
            self.check_result(err=b"AGGREGATE_FIELD_TYPE_UNVERIFIED\ndefine i64 @main() {}")

    def test_zero_exit_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "normally"):
            self.check_result(rc=0)

    def test_signal_exit_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "normally"):
            self.check_result(rc=-9)

    def test_plain_text_context_accepted(self):
        self.check_result(err=b"error: AGGREGATE_FIELD_TYPE_UNVERIFIED at byte 31")

    def test_empty_stderr_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "complete identifier"):
            self.check_result(err=b"")


if __name__ == "__main__":
    unittest.main()
