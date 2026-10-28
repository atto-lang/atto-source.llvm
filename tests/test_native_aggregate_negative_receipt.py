#!/usr/bin/env python3
"""Source-free negative compiler-evidence harness adversarial tests."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from native_aggregate_negative_receipt import verify_negative_case


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
            self.verify(stderr="AGGREGATE_SHAPE_DUPLICATE_FIELD\ndefine i64 @main() {}")

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


if __name__ == "__main__":
    unittest.main()
