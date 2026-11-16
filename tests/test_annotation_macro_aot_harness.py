#!/usr/bin/env python3
"""Offline regression: rejected LLVM compilations cannot leak sidecar files."""
import pathlib
import sys
import tempfile
import unittest

from annotation_macro_aot_admission_matrix import BASE, invoke, common_annotation_vectors


class AnnotationAotHarnessTest(unittest.TestCase):
    def test_shared_corpus_is_complete_and_closed(self):
        denied, allowed = common_annotation_vectors()
        self.assertGreaterEqual(len(denied) + len(allowed), 12)
        self.assertIn("parity_caller_role", denied)
        self.assertIn("parity_unknown_annotation", denied)
        self.assertIn("parity_leading_comment", allowed)
        self.assertEqual(len(set(denied) & set(allowed)), 0)

    def test_rejected_compile_records_sidecar_even_without_stdout(self):
        with tempfile.TemporaryDirectory(prefix="oreslang-aot-harness-test-") as tmp:
            compiler = pathlib.Path(tmp) / "fake_compiler"
            compiler.write_text(
                "#!" + sys.executable + "\n"
                "from pathlib import Path\n"
                "import sys\n"
                "Path('unverified.ll').write_text('define i64 @main() { ret i64 42 }')\n"
                "sys.stderr.write('rejected syntax\\n')\n"
                "sys.exit(1)\n",
                encoding="utf-8")
            compiler.chmod(0o755)
            result, emitted = invoke(str(compiler), tmp, "negative_fake",
                                     "@derive(Eq)\n" + BASE, "host")
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")
            self.assertIn("unverified.ll", emitted)

    def test_successful_control_writes_no_sidecars(self):
        with tempfile.TemporaryDirectory(prefix="oreslang-aot-harness-test-") as tmp:
            compiler = pathlib.Path(tmp) / "fake_compiler"
            compiler.write_text(
                "#!" + sys.executable + "\n"
                "print('define i64 @main() { ret i64 42 }')\n",
                encoding="utf-8")
            compiler.chmod(0o755)
            result, emitted = invoke(str(compiler), tmp, "positive_fake",
                                     BASE, "host")
            self.assertEqual(result.returncode, 0)
            self.assertEqual(emitted, [])
            self.assertIn("define i64 @main", result.stdout)


if __name__ == "__main__":
    unittest.main()
