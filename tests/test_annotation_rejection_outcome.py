#!/usr/bin/env python3
"""Source-only admission checker tests; real host/wasm32 compiler runs remain required."""
import importlib.util
import pathlib
import subprocess
import unittest

MODULE = pathlib.Path(__file__).with_name("annotation_macro_aot_admission_matrix.py")
SPEC = importlib.util.spec_from_file_location("annotation_aot_matrix", MODULE)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def result(code=1, stdout="", stderr="error: unsupported annotation"):
    return subprocess.CompletedProcess(args=["compiler"], returncode=code,
                                       stdout=stdout, stderr=stderr)


class RejectedCompilerOutcomeTest(unittest.TestCase):
    def test_semantic_diagnostic_is_a_valid_rejection(self):
        self.assertEqual([], gate.rejected_compiler_outcome(result(), [], "case"))

    def test_success_does_not_count_as_rejection(self):
        self.assertTrue(gate.rejected_compiler_outcome(result(code=0), [], "case"))

    def test_signal_termination_is_not_a_semantic_rejection(self):
        for status in (-11, -6, 134, 139, 255):
            with self.subTest(status=status):
                self.assertTrue(gate.rejected_compiler_outcome(
                    result(code=status), [], "case"))

    def test_crash_markers_are_not_valid_rejections_even_with_exit_one(self):
        for message in (
            "internal compiler error: bug",
            "thread 'main' panicked at src/lib.rs",
            "stack backtrace:",
            "Traceback (most recent call last):",
            "Segmentation fault (core dumped)",
            "assertion failed",
        ):
            with self.subTest(message=message):
                self.assertTrue(gate.rejected_compiler_outcome(
                    result(stderr=message), [], "case"))

    def test_partial_ir_or_sidecar_publication_is_rejected(self):
        self.assertTrue(gate.rejected_compiler_outcome(
            result(stdout="define i64 @main() { ret i64 42 }"), [], "case"))
        self.assertTrue(gate.rejected_compiler_outcome(
            result(), ["guest.ll"], "case"))

    def test_empty_diagnostic_does_not_count_as_rejection(self):
        self.assertTrue(gate.rejected_compiler_outcome(
            result(stderr=""), [], "case"))

    def test_doc_annotation_rejections_cover_new_compiler_frontends(self):
        self.assertIn("doc_class_unlowered", gate.TARGET_ANNOTATION_CASES)
        self.assertIn("doc_actor_unlowered", gate.TARGET_ANNOTATION_CASES)
        self.assertIn("doc_method_unlowered", gate.TARGET_ANNOTATION_CASES)


if __name__ == "__main__":
    unittest.main()
