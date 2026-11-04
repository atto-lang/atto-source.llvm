#!/usr/bin/env python3
"""Negative-path tests for the DOP AOT verifier; NOT native guest evidence.

All tools below are deliberately synthetic and run only in an isolated temp
directory. Every test expects refusal, never a positive guest execution.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


VERIFIER = Path(__file__).resolve().with_name("verify-dop-atto-aot.sh")


class FailClosedVerifierTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="atto-dop-negative-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        fixture = self.root / "examples" / "dop-cache"
        fixture.mkdir(parents=True)
        self.source = fixture / "soa-int-update.atto"
        self.source.write_text("pub routine main(): int { return 0; }\n", encoding="utf-8")
        shutil.copyfile(VERIFIER, scripts / "verify-dop-atto-aot.sh")
        self.compiler = self.root / "test-compiler"
        self.clang = self.root / "test-clang"
        for tool in (self.compiler, self.clang):
            tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            tool.chmod(0o700)
        compiler_stub = scripts / "compile-aot.sh"
        compiler_stub.write_text(
            "#!/bin/bash\n"
            "set -euo pipefail\n"
            "case \"$DOP_NEGATIVE_CASE\" in\n"
            "  build_error) exit 23 ;;\n"
            "  no_binary) exit 0 ;;\n"
            "  bad_guest) printf '#!/bin/sh\\nexit 17\\n' > \"$2\" ;;\n"
            "  source_mutated) printf '// mutation\\n' >> \"$1\" ;;\n"
            "  compiler_mutated) printf '# mutation\\n' >> \"$ORESLANG_LLVMC\" ;;\n"
            "  clang_mutated) printf '# mutation\\n' >> \"$CLANG\" ;;\n"
            "  *) exit 90 ;;\n"
            "esac\n"
            "if [[ \"$DOP_NEGATIVE_CASE\" != no_binary ]]; then\n"
            "  if [[ \"$DOP_NEGATIVE_CASE\" != bad_guest ]]; then\n"
            "    printf '#!/bin/sh\\nexit 0\\n' > \"$2\"\n"
            "  fi\n"
            "  chmod 0700 \"$2\"\n"
            "fi\n",
            encoding="utf-8",
        )
        self.env = dict(os.environ, ORESLANG_LLVMC=str(self.compiler),
                        CLANG=str(self.clang), DOP_NEGATIVE_CASE="")

    def run_negative(self, case=None, **overrides):
        env = dict(self.env, **overrides)
        if case:
            env["DOP_NEGATIVE_CASE"] = case
        result = subprocess.run(
            ["bash", str(self.root / "scripts" / "verify-dop-atto-aot.sh")],
            env=env, capture_output=True, text=True, timeout=12,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("PASS: native LLVM/Clang guest", result.stdout)
        return result

    def test_missing_fixture_is_refused(self):
        self.source.unlink()
        self.assertEqual(self.run_negative().returncode, 66)

    def test_symlinked_fixture_is_refused(self):
        target = self.root / "outside.atto"
        target.write_text("pub routine main(): int { return 0; }\n", encoding="utf-8")
        self.source.unlink()
        self.source.symlink_to(target)
        self.assertEqual(self.run_negative().returncode, 66)

    def test_missing_compiler_is_not_executed(self):
        result = self.run_negative(ORESLANG_LLVMC=str(self.root / "missing"))
        self.assertEqual(result.returncode, 69)
        self.assertIn("compiler is unavailable", result.stderr)

    def test_missing_clang_is_not_executed(self):
        result = self.run_negative(CLANG=str(self.root / "missing"))
        self.assertEqual(result.returncode, 69)
        self.assertIn("Clang native linker is unavailable", result.stderr)

    def test_compile_failure_is_refused(self):
        result = self.run_negative("build_error")
        self.assertIn("compilation/link failed or timed out", result.stderr)

    def test_missing_native_output_is_refused(self):
        result = self.run_negative("no_binary")
        self.assertIn("native output was not executable", result.stderr)

    def test_nonzero_native_exit_is_refused(self):
        result = self.run_negative("bad_guest")
        self.assertIn("guest returned nonzero exit status 17", result.stderr)

    def test_mutated_source_is_refused(self):
        result = self.run_negative("source_mutated")
        self.assertIn(".atto guest source changed", result.stderr)

    def test_mutated_compiler_is_refused(self):
        result = self.run_negative("compiler_mutated")
        self.assertIn("LLVM guest compiler changed", result.stderr)

    def test_mutated_clang_is_refused(self):
        result = self.run_negative("clang_mutated")
        self.assertIn("Clang changed", result.stderr)


if __name__ == "__main__":
    unittest.main()
