#!/usr/bin/env python3
"""Source-free negative compiler-evidence harness adversarial tests."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from native_aggregate_negative_receipt import (
    complete_negative_receipt, git_blob_sha1, result_digest,
    verify_negative_case, verify_scalar_positive_control, verify_isolated_workspace,
)


class NegativeReceiptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = Path(self.tmp.name) / "case.ores"
        self.original = b"fnc f(): int { return 1; }\n"
        self.source.write_bytes(self.original)

    def verify(self, rc=1, stdout="", stderr="AGGREGATE_SHAPE_DUPLICATE_FIELD"):
        result = subprocess.CompletedProcess(["compiler"], rc, stdout, stderr)
        verify_negative_case("case", self.source, self.original,
                             "AGGREGATE_SHAPE_DUPLICATE_FIELD", result)

    def test_real_failure_with_exact_diagnostic_and_no_artifacts(self):
        self.verify()

    def test_zero_exit_rejected(self):
        with self.assertRaisesRegex(AssertionError, "did not fail"):
            self.verify(rc=0)

    def test_signal_killed_compiler_rejected(self):
        with self.assertRaisesRegex(AssertionError, "did not fail"):
            self.verify(rc=-9)

    def test_diagnostic_only_in_stdout_rejected(self):
        with self.assertRaisesRegex(AssertionError, "expected diagnostic"):
            self.verify(stdout="AGGREGATE_SHAPE_DUPLICATE_FIELD", stderr="")

    def test_diagnostic_with_ir_on_stdout_rejected(self):
        with self.assertRaisesRegex(AssertionError, "stdout"):
            self.verify(stdout="; ModuleID = 'illicit'\n")

    def test_diagnostic_with_ir_on_stderr_rejected(self):
        with self.assertRaisesRegex(AssertionError, "LLVM IR"):
            self.verify(stderr="AGGREGATE_SHAPE_DUPLICATE_FIELD\ndefine i64 @__oreslang_guest_main() {}")

    def test_object_file_leak_rejected(self):
        (self.source.parent / "case.o").write_bytes(b"not permitted")
        with self.assertRaisesRegex(AssertionError, "left artifacts"):
            self.verify()

    def test_wasm_file_leak_rejected(self):
        (self.source.parent / "case.wasm").write_bytes(b"not permitted")
        with self.assertRaisesRegex(AssertionError, "left artifacts"):
            self.verify()

    def test_arbitrary_artifact_leak_rejected(self):
        (self.source.parent / "opaque-output").write_bytes(b"not permitted")
        with self.assertRaisesRegex(AssertionError, "left artifacts"):
            self.verify()

    def test_source_mutation_rejected(self):
        self.source.write_bytes(b"changed")
        with self.assertRaisesRegex(AssertionError, "mutated"):
            self.verify()

    def test_symlink_source_rejected(self):
        original = self.source.parent / "original.ores"
        original.write_bytes(self.original)
        self.source.unlink()
        self.source.symlink_to(original)
        with self.assertRaisesRegex(AssertionError, "mutated"):
            self.verify()


class ScalarPositiveControlTest(unittest.TestCase):
    """The negative suite must be unable to pass with a reject-all compiler."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = Path(self.tmp.name) / "scalar.ores"
        self.original = b"pub fnc main(): int { return 42; }" + bytes([10])
        self.source.write_bytes(self.original)

    def verify(self, rc=0, stdout="; ModuleID = 'scalar'\ndefine i64 @__oreslang_guest_main() {\n  ret i64 42\n}\ndefine i32 @main() {\n  ret i32 42\n}\n",
               stderr=""):
        result = subprocess.CompletedProcess(["compiler"], rc, stdout, stderr)
        verify_scalar_positive_control("scalar", self.source, self.original, result)

    def test_real_success_with_42_ir(self):
        self.verify()

    def test_reject_all_compiler_does_not_pass(self):
        with self.assertRaisesRegex(AssertionError, "did not compile"):
            self.verify(rc=1, stdout="", stderr="AGGREGATE_LOWERING_UNVERIFIED")

    def test_no_ir_does_not_pass(self):
        with self.assertRaisesRegex(AssertionError, "expected 42 IR"):
            self.verify(stdout="")

    def test_wrong_result_does_not_pass(self):
        with self.assertRaisesRegex(AssertionError, "expected 42 IR"):
            self.verify(stdout="; ModuleID = 'scalar'\ndefine i64 @__oreslang_guest_main() {\n  ret i64 7\n}\ndefine i32 @main() {\n  ret i32 42\n}\n")

    def test_stderr_does_not_pass(self):
        with self.assertRaisesRegex(AssertionError, "emitted stderr"):
            self.verify(stderr="warning")

    def test_object_artifact_does_not_pass(self):
        (self.source.parent / "bad.o").write_bytes(b"unexpected artifact")
        with self.assertRaisesRegex(AssertionError, "left artifacts"):
            self.verify()

    def test_replaced_source_does_not_pass(self):
        self.source.write_bytes(b"bad source")
        with self.assertRaisesRegex(AssertionError, "mutated"):
            self.verify()

    def test_signal_killed_control_does_not_pass(self):
        with self.assertRaisesRegex(AssertionError, "did not compile"):
            self.verify(rc=-9)


class IsolatedWorkspaceTest(unittest.TestCase):
    """A sibling or parent-directory IR emission must fail the gate."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.case = self.root / "case"
        self.case.mkdir()
        self.source = self.case / "case.ores"
        self.original = b"source" + bytes([10])
        self.source.write_bytes(self.original)
        self.expected = {"case": (self.source, self.original)}

    def test_clean_workspace(self):
        verify_isolated_workspace(self.root, self.expected)

    def test_ir_escaped_into_workspace_root(self):
        (self.root / "escaped.ll").write_bytes(b"define @bad")
        with self.assertRaisesRegex(AssertionError, "unexpected entries"):
            verify_isolated_workspace(self.root, self.expected)

    def test_unknown_sibling_case(self):
        (self.root / "other").mkdir()
        with self.assertRaisesRegex(AssertionError, "unexpected entries"):
            verify_isolated_workspace(self.root, self.expected)

    def test_artifact_inside_prior_case(self):
        (self.case / "object.o").write_bytes(b"object")
        with self.assertRaisesRegex(AssertionError, "contains artifacts"):
            verify_isolated_workspace(self.root, self.expected)

    def test_prior_case_source_mutation(self):
        self.source.write_bytes(b"mutated")
        with self.assertRaisesRegex(AssertionError, "source changed"):
            verify_isolated_workspace(self.root, self.expected)

    def test_symlink_directory_replacement(self):
        alternate = self.root / "alternate"
        alternate.mkdir()
        self.source.unlink()
        self.case.rmdir()
        self.case.symlink_to(alternate, target_is_directory=True)
        self.expected = {"case": (self.source, self.original),
                         "alternate": (alternate / "case.ores", self.original)}
        with self.assertRaisesRegex(AssertionError, "source changed"):
            verify_isolated_workspace(self.root, self.expected)


if __name__ == "__main__":
    unittest.main()
