#!/usr/bin/env python3
"""A -O / PYTHONOPTIMIZE run must enforce, not erase, native contract checks."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ContractHarnessOptimizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = Path(__file__).with_name("contract.py")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ores-contract-opt-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = self.root / "fixture.ores"
        self.fixture.write_text("fixture", encoding="utf-8")

    def compiler(self, *, status=0, stdout="", stderr=""):
        path = self.root / "fake-compiler"
        path.write_text(
            "#!" + sys.executable + "\n"
            + "import sys\n"
            + f"sys.stdout.write({stdout!r})\n"
            + f"sys.stderr.write({stderr!r})\n"
            + f"sys.exit({status})\n",
            encoding="utf-8",
        )
        path.chmod(0o700)
        return path

    def run_contract(self, compiler, *flags, optimize="-O"):
        env = dict(os.environ, PYTHONOPTIMIZE="2")
        return subprocess.run(
            [sys.executable, optimize, str(self.harness),
             "--compiler", str(compiler), "--fixture", str(self.fixture), *flags],
            capture_output=True, text=True, check=False, timeout=15, env=env,
        )

    def test_valid_negative_still_passes_under_optimization(self):
        compiler = self.compiler(status=1, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_rejection_must_not_accept_successful_compilation(self):
        compiler = self.compiler(status=0, stdout="; ModuleID = 'wrong'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("invalid source unexpectedly accepted", p.stderr)

    def test_rejection_must_not_accept_missing_diagnostic(self):
        compiler = self.compiler(status=1, stderr="wrong diagnostic")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)

    def test_rejection_must_not_emit_any_ir(self):
        compiler = self.compiler(status=1, stderr="private class field",
                                 stdout="; ModuleID = 'bad'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("emitted IR after rejection", p.stderr)

    def test_valid_positive_still_passes_under_optimization(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n")
        p = self.run_contract(compiler)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_positive_checks_bad_status_and_ir(self):
        bad_status = self.compiler(status=1, stderr="compilation failed")
        self.assertNotEqual(self.run_contract(bad_status).returncode, 0)
        bad_ir = self.compiler(stdout="not LLVM IR")
        self.assertNotEqual(self.run_contract(bad_ir).returncode, 0)

    def test_positive_rejects_stderr_and_forbidden_abi(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n",
                                 stderr="unexpected warning")
        self.assertNotEqual(self.run_contract(compiler).returncode, 0)
        compiler = self.compiler(stdout="; ModuleID = 'good'\n"
                                 "declare void @oreslang_owned_i64_region_fake()\n")
        self.assertNotEqual(
            self.run_contract(compiler, "--no-native-abi").returncode, 0
        )

    def test_required_ir_and_native_run_flags_remain_checked(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n")
        self.assertNotEqual(
            self.run_contract(compiler, "--ir", "define i32 @main").returncode, 0
        )
        self.assertNotEqual(
            self.run_contract(compiler, "--run", "42").returncode, 0
        )

    def test_double_optimized_python_also_enforces_rejection(self):
        compiler = self.compiler(status=0, stdout="; ModuleID = 'wrong'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field",
                              optimize="-OO")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
