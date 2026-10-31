"""Source-free bounded CGL smoke regression using synthetic compiler/ELF shims.

This is NOT a real compiler/ELF test. Actual native acceptance is separate.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SMOKE = ROOT / "scripts" / "cgl-native-smoke.sh"


class CGLSmokeBounds(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cgl-smoke-sourcefree-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for subdir in ("scripts", "build", "tests/fixtures", "bin"):
            (self.root / subdir).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SMOKE, self.root / "scripts/cgl-native-smoke.sh")
        (self.root / "build/oreslang-llvmc").write_text("#!/bin/sh\nexit 0\n")
        (self.root / "build/oreslang-llvmc").chmod(0o755)
        (self.root / "scripts/cgl-preflight.py").write_text("import sys\nsys.exit(0)\n")
        (self.root / "tests/fixtures/forward-call-main.ores").write_text("// synthetic\n")
        compiler = self.root / "scripts/compile-aot.sh"
        compiler.write_text(
            '#!/usr/bin/env bash\n'
            'set -euo pipefail\n'
            'out="${@: -1}"\n'
            'printf "#!/bin/sh\\nexit %s\\n" "${MOCK_EXIT_CODE:-42}" > "$out"\n'
            'chmod 755 "$out"\n'
        )
        compiler.chmod(0o755)

    def run_smoke(self, *, exit_code="42", fake_timeout=False):
        env = os.environ.copy()
        env["MOCK_EXIT_CODE"] = exit_code
        if fake_timeout:
            wrapper = self.root / "bin/timeout"
            wrapper.write_text(
                '#!/usr/bin/env bash\n'
                'if [[ "$*" != *" env ORESLANG_LLVMC="* && "${*: -1}" == */cgl-scalar ]]; then exit 124; fi\n'
                'exec /usr/bin/timeout "$@"\n'
            )
            wrapper.chmod(0o755)
            env["PATH"] = str(self.root / "bin") + os.pathsep + env.get("PATH", "")
        return subprocess.run(
            ["bash", str(self.root / "scripts/cgl-native-smoke.sh")],
            cwd=self.root, env=env, text=True, capture_output=True, timeout=15,
            check=False,
        )

    def test_real_timeout_commands_present(self):
        code = SMOKE.read_text()
        self.assertIn('timeout --signal=TERM --kill-after=5s 180s env ORESLANG_LLVMC=', code)
        self.assertIn('timeout --signal=TERM --kill-after=5s 30s "$work/cgl-scalar"', code)

    def test_synthetic_guest_exit_42_passes(self):
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CGL PILOT SMOKE PASS", result.stdout)

    def test_synthetic_wrong_exit_fails(self):
        result = self.run_smoke(exit_code="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected native source program exit 42", result.stderr)

    def test_synthetic_deadline_expiry_fails(self):
        result = self.run_smoke(fake_timeout=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("got 124", result.stderr)


if __name__ == "__main__":
    unittest.main()
