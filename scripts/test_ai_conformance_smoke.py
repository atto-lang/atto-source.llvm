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
            self.result(1, "error: disk full\nsource text: nonexistent_symbol"),
            self.result(1, "nonexistent_symbol\nunrelated error: disk full"),
            self.result(1, "Traceback: error nonexistent_symbol"),
            self.result(1, "error: nonexistent_symbol", truncated=True),
            self.result(None, "error: nonexistent_symbol", timed_out=True),
        ]:
            self.assertFalse(smoke.rejected(result))

    def test_reference_programs_stable(self):
        self.assertIn("return 20 + 22;", smoke.POSITIVE)
        self.assertIn("nonexistent_symbol", smoke.NEGATIVE)
        self.assertNotIn("nonexistent_symbol", smoke.POSITIVE)

    def test_checkout_identity_reads_current_git_head(self):
        import subprocess
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "init", "-q", tmp], check=True)
            subprocess.run(["git", "-C", tmp, "-c", "user.name=Conformance",
                            "-c", "user.email=conformance@example.invalid",
                            "commit", "-q", "--allow-empty", "-m", "fixture"], check=True)
            actual = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"],
                                    capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(smoke.git_checkout_sha(Path(tmp)), actual)
            self.assertRegex(actual, r"^[0-9a-f]{40}$")

    def test_fake_claimed_source_sha_is_rejected_before_compiler_use(self):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([
                sys.executable, str(script), "--backend=llvm",
                "--compiler=" + sys.executable, "--clang=" + sys.executable,
                "--source-sha=" + ("0" * 40),
                "--report=" + str(Path(temp) / "report.json"),
            ], capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("declared source revision does not match Git checkout HEAD",
                          result.stderr)
            self.assertFalse((Path(temp) / "report.json").exists())

    def test_compiler_digest_reads_actual_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "program"
            file.write_bytes(b"abc")
            self.assertEqual(
                smoke.sha256(file),
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            )

    def test_process_capture_is_bounded_and_executes_no_shell(self):
        import sys
        from pathlib import Path
        cmd = [
            sys.executable, "-c",
            "import sys;sys.stdout.write('x'*70000);sys.stdout.flush()",
        ]
        result = smoke.run(cmd, 5, Path(__file__).resolve().parent)
        self.assertTrue(result["output_truncated"])
        self.assertFalse(result["timeout"])
        self.assertLessEqual(len(result["stdout"].encode()), smoke.MAX_OUTPUT)
        self.assertFalse(smoke.good(result))

    def test_timeout_stops_launcher_and_descendants(self):
        import sys
        from pathlib import Path
        # An inner child retains the inherited pipe writers if a launcher
        # crashes or hangs. A process-group timeout must still terminate.
        cmd = [
            sys.executable, "-c",
            "import subprocess,sys,time;"
            "subprocess.Popen([sys.executable,'-c','import time;time.sleep(12)']);"
            "time.sleep(12)",
        ]
        result = smoke.run(cmd, 0.25, Path(__file__).resolve().parent)
        self.assertTrue(result["timeout"])
        self.assertFalse(smoke.good(result))
        self.assertLess(result["duration_ms"], 5000)

    def test_missing_compiler_fails_closed(self):
        from pathlib import Path
        result = smoke.run(["/definitely/nonexistent/oreslangc", "input.ores"], 1, Path(__file__).resolve().parent)
        self.assertIsNone(result["exit_code"])
        self.assertFalse(smoke.good(result))
        self.assertFalse(smoke.rejected(result))


if __name__ == "__main__":
    unittest.main()
