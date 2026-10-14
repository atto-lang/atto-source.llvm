#!/usr/bin/env python3
"""Unit tests for smoke-report admission, not compiler conformance."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

script = Path(__file__).with_name("ai-conformance-smoke.py")
spec = importlib.util.spec_from_file_location("ai_conformance_smoke", script)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class SmokeAdmission(unittest.TestCase):
    def result(self, code, message="", *, timed_out=False, truncated=False):
        return {
            "exit_code": code, "stdout": "", "stderr": message,
            "timeout": timed_out, "output_truncated": truncated,
        }

    def test_positive_requires_zero_exit_and_complete_output(self):
        self.assertTrue(smoke.good(self.result(0)))
        for result in [
            self.result(1), self.result(None), self.result(0, timed_out=True),
            self.result(0, truncated=True),
        ]:
            self.assertFalse(smoke.good(result))

    def test_negative_requires_actual_diagnostic_and_symbol(self):
        self.assertTrue(smoke.rejected(self.result(1, "error: unknown identifier nonexistent_symbol")))
        for result in [
            self.result(0, "error: unknown identifier nonexistent_symbol"),
            self.result(1, "error: unknown identifier"),
            self.result(1, "nonexistent_symbol"),
            self.result(1, "Traceback: error nonexistent_symbol"),
            self.result(1, "error: nonexistent_symbol", truncated=True),
            self.result(None, "error: nonexistent_symbol", timed_out=True),
        ]:
            self.assertFalse(smoke.rejected(result))

    def test_reference_programs_stable(self):
        self.assertIn("return 20 + 22;", smoke.POSITIVE)
        self.assertIn("nonexistent_symbol", smoke.NEGATIVE)
        self.assertNotIn("nonexistent_symbol", smoke.POSITIVE)

    def test_compiler_digest_reads_actual_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "program"
            file.write_bytes(b"abc")
            self.assertEqual(
                smoke.sha256(file),
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            )


if __name__ == "__main__":
    unittest.main()
