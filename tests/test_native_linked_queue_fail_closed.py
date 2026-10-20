"""Adversarial checks: the fail-closed harness must not accept false positives."""
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from native_linked_queue_fail_closed import git_blob_sha, verify_fixture, verify_rejection


class NativeLinkedQueueHarnessTest(unittest.TestCase):
    expected = "oreslang-llvmc: expected fnc or routine at byte 156"

    def result(self, status=1, out=b"", err=None):
        if err is None:
            err = (self.expected + "\n").encode("utf-8")
        return subprocess.CompletedProcess(["compiler", "fixture"], status, out, err)

    def test_expected_rejection_is_admitted(self):
        verify_rejection("smoke", self.result(), self.expected)

    def test_stub_that_rejects_everything_with_generic_error_fails(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(err=b"oreslang-llvmc: internal error\n"),
                             self.expected)

    def test_missing_source_is_not_unsupported_language(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(err=b"oreslang-llvmc: cannot open Oreslang source\n"),
                             self.expected)

    def test_cli_misuse_is_not_language_rejection(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(status=2), self.expected)

    def test_crash_is_not_language_rejection(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(status=-11), self.expected)

    def test_success_without_ir_is_not_rejection(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(status=0), self.expected)

    def test_stdout_with_only_whitespace_still_fails(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(out=b"\n"), self.expected)

    def test_partial_ir_still_fails(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(out=b"; ModuleID = 'oops'\n"),
                             self.expected)

    def test_extra_diagnostic_output_fails(self):
        with self.assertRaises(AssertionError):
            verify_rejection("smoke", self.result(err=(self.expected + "\nwarning\n").encode()),
                             self.expected)

    def test_blob_is_git_hash_not_plain_sha1(self):
        data = b"abc"
        expected = hashlib.sha1(b"blob 3\0abc").hexdigest()
        self.assertEqual(git_blob_sha(data), expected)

    def test_fixture_mutation_fails_integrity(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "example.ores"
            src.write_bytes(b"original")
            sha = git_blob_sha(src.read_bytes())
            verify_fixture(src, sha)
            src.write_bytes(b"original\n")
            with self.assertRaises(AssertionError):
                verify_fixture(src, sha)

    def test_empty_or_missing_fixture_fails_integrity(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "example.ores"
            with self.assertRaises(AssertionError):
                verify_fixture(src, "0000")
            src.write_bytes(b"")
            with self.assertRaises(AssertionError):
                verify_fixture(src, "0000")


if __name__ == "__main__":
    unittest.main()
