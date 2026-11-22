"""Host-only proof that the negative guest artifact evidence gate fails closed."""
import tempfile
from pathlib import Path
import unittest

from guard_artifact_evidence import require_no_rejected_artifacts


class RejectedArtifactGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="atto-artifact-gate-")
        self.addCleanup(self.tmp.cleanup)
        self.case_dir = Path(self.tmp.name)
        self.source = self.case_dir / "input.atto"
        self.source.write_text("pub fnc main(): int { ^; }\n", encoding="utf-8")

    def gate(self, stdout=b""):
        require_no_rejected_artifacts(self.case_dir, self.source, stdout)

    def test_clean_rejection(self):
        self.gate()
        self.gate(b"\n  \t")

    def test_stdout_leak_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, "stdout"):
            self.gate(b"define i32 @main()")

    def test_partial_ir_file_is_rejected(self):
        (self.case_dir / "input.ll").write_text("define i32 @main() { ret i32 0 }\n")
        with self.assertRaisesRegex(AssertionError, "input.ll"):
            self.gate()

    def test_nested_artifact_directory_is_rejected(self):
        (self.case_dir / "out").mkdir()
        (self.case_dir / "out" / "input.o").write_bytes(b"obj")
        with self.assertRaisesRegex(AssertionError, "out"):
            self.gate()

    def test_symlinked_output_is_rejected(self):
        (self.case_dir / "input.o").symlink_to(self.source)
        with self.assertRaisesRegex(AssertionError, "input.o"):
            self.gate()

    def test_symlinked_input_is_rejected(self):
        self.source.unlink()
        outside = self.case_dir.parent / self.case_dir.name.join(["", "-other.atto"])
        outside.write_text("test", encoding="utf-8")
        self.addCleanup(outside.unlink)
        self.source.symlink_to(outside)
        with self.assertRaisesRegex(AssertionError, "regular local file"):
            self.gate()

    def test_missing_input_is_rejected(self):
        self.source.unlink()
        with self.assertRaisesRegex(AssertionError, "regular local file"):
            self.gate()


if __name__ == "__main__":
    unittest.main()
