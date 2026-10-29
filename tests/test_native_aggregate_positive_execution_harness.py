#!/usr/bin/env python3
"""Source-free adversarial tests for native exact-struct guest admission.

These are NOT .ores compiler executions or evidence of LLVM guest parity.
"""
import subprocess
import unittest

from native_aggregate_positive_execution import verify_negative_rejection


class NativeAggregatePositiveHarnessTest(unittest.TestCase):
    diagnostic = "AGGREGATE_FIELD_TYPE_UNVERIFIED"

    def check_result(self, rc=1, out=b"", err=b"AGGREGATE_FIELD_TYPE_UNVERIFIED"):
        result = subprocess.CompletedProcess(["synthetic-compiler"], rc, out, err)
        verify_negative_rejection(result, self.diagnostic)

    def test_exact_diagnostic_accepted(self):
        self.check_result()

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
