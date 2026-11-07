#!/usr/bin/env python3
"""Adversarial checks for the *real* mixed guest process verifier.

These execute the embedded Python harness taken from its actual shell source.
Passing them is HOST-ONLY infrastructure evidence, never AttoLang guest parity.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).with_name("native_inverted_bool_i64_guest.sh")
HEREDOC = '''python3 - "$1" "$2" "$3" "$4" <<'PY'\n'''


class NativeGuestEvidenceNegativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        shell = SOURCE.read_text(encoding="utf-8")
        if shell.count(HEREDOC) != 1:
            raise AssertionError("cannot locate exactly one native guest verifier")
        cls.guest_verifier = shell.split(HEREDOC, 1)[1].split("\nPY\n", 1)[0]
        for required in (
                "resource.RLIMIT_FSIZE", "resource.RLIMIT_CPU",
                "hashlib.sha256", "guest.is_symlink()",
                "bytes.fromhex", "subprocess.run",
                "timeout=15", "result.returncode"):
            if required not in cls.guest_verifier:
                raise AssertionError(f"missing required native verifier check: {required}")
        cls.clang = shutil.which("clang")
        if not cls.clang:
            raise AssertionError("Clang required; never silently skip native negative tests")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="atto-native-evidence-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def build(self, program, optimization="-O0"):
        source = self.root / "guest.c"
        binary = self.root / "guest"
        source.write_text(program, encoding="utf-8")
        subprocess.run([self.clang, optimization, str(source), "-o", str(binary)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=20)
        return binary

    def check(self, binary, expected="exit42"):
        return subprocess.run(
            [sys.executable, "-c", self.guest_verifier,
             str(binary), expected, "harness-negative-control", "-O0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=20, check=False
        )

    def test_actual_elf_exit42_O0(self):
        result = self.check(self.build("int main(void){return 42;}", "-O0"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ATTO_MIXED_GUEST_EXECUTABLE_SHA256=", result.stdout)

    def test_actual_elf_exit42_O2(self):
        result = self.check(self.build("int main(void){return 42;}", "-O2"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_elf_trap_signal(self):
        result = self.check(self.build("int main(void){__builtin_trap();}", "-O2"),
                            "signal")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_shell_script_impersonating_native_guest_rejected(self):
        script = self.root / "forged"
        script.write_text("#!/bin/sh\nexit 42\n", encoding="utf-8")
        script.chmod(0o755)
        result = self.check(script)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a native ELF/Mach-O", result.stderr)

    def test_symlink_to_real_elf_rejected(self):
        binary = self.build("int main(void){return 42;}")
        link = self.root / "link"
        link.symlink_to(binary)
        result = self.check(link)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("real executable regular file", result.stderr)

    def test_nonexecuting_elf_magic_rejected(self):
        forged = self.root / "forged-elf"
        forged.write_bytes(bytes.fromhex("7f454c46") + b"\0" * 64)
        forged.chmod(0o755)
        result = self.check(forged)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("guest not executed", result.stderr)

    def test_wrong_exit_rejected(self):
        result = self.check(self.build("int main(void){return 0;}"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected 42", result.stderr)

    def test_nontrap_exit_rejected(self):
        result = self.check(self.build("int main(void){return 42;}"), "signal")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no real trap signal", result.stderr)

    def test_unexpected_stdout_rejected(self):
        result = self.check(self.build(
            '#include <stdio.h>\nint main(void){puts("forged");return 42;}'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpectedly wrote stdout/stderr", result.stderr)

    def test_unexpected_stderr_rejected(self):
        result = self.check(self.build(
            '#include <stdio.h>\nint main(void){fputs("forged",stderr);return 42;}'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpectedly wrote stdout/stderr", result.stderr)

    def test_missing_executable_rejected(self):
        result = self.check(self.root / "does-not-exist")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("real executable regular file", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
