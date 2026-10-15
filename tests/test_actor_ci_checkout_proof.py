"""Smoke-test checkout identity admission; mocked toolchains do not compile guest code."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHOICES = ("verify_actor_exception_jvm.sh", "verify_actor_control_host.sh")

@unittest.skipUnless(shutil.which("bash") and shutil.which("git"), "requires bash and git")
class CheckoutProofTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "checkout"
        (self.repo / "scripts").mkdir(parents=True)
        self.bin = Path(self.temp.name) / "bin"
        self.bin.mkdir()
        found = [s for s in CHOICES if (ROOT / "scripts" / s).is_file()]
        if len(found) != 1:
            self.skipTest("expected exactly one actor verifier script")
        self.script = found[0]
        shutil.copyfile(ROOT / "scripts" / self.script,
                        self.repo / "scripts" / self.script)
        for name in ("java", "mvn"):
            self.tool(name, "#!/bin/sh\nexit 0\n")
        self.tool("fake-cxx", """#!/bin/bash
set -e
while (($#)); do
  if [[ "$1" == "-o" ]]; then
    printf '#!/bin/sh\\nexit 0\\n' > "$2"
    chmod +x "$2"
    exit 0
  fi
  shift
done
exit 97
""")
        for args in (("init", "-q"), ("config", "user.email", "test@example.invalid"),
                     ("config", "user.name", "Test"), ("add", "scripts"),
                     ("commit", "-qm", "fixture")):
            subprocess.run(("git", *args), cwd=self.repo, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.sha = subprocess.check_output(
            ("git", "rev-parse", "HEAD"), cwd=self.repo, text=True).strip()

    def tool(self, name, contents):
        path = self.bin / name
        path.write_text(contents)
        path.chmod(0o755)

    def run_verifier(self, sha):
        env = os.environ.copy()
        env["PATH"] = str(self.bin) + os.pathsep + env.get("PATH", "")
        env["CXX"] = str(self.bin / "fake-cxx")
        env["ORESLANG_EXPECTED_SOURCE_SHA"] = sha
        return subprocess.run(
            ("bash", str(self.repo / "scripts" / self.script)), cwd=self.repo,
            env=env, text=True, capture_output=True, timeout=15)

    def test_pinned_clean_tree_admitted(self):
        result = self.run_verifier(self.sha)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(self.sha, result.stderr)

    def test_wrong_sha_denied(self):
        result = self.run_verifier("0" * 40 if self.sha != "0" * 40 else "1" * 40)
        self.assertEqual(2, result.returncode)
        self.assertIn("Exact-head verification refused", result.stderr)

    def test_invalid_sha_denied(self):
        self.assertEqual(2, self.run_verifier("not-a-sha").returncode)

    def test_dirty_tracked_file_denied(self):
        with (self.repo / "scripts" / self.script).open("a") as f:
            f.write("\n# modified\n")
        result = self.run_verifier(self.sha)
        self.assertEqual(2, result.returncode)
        self.assertIn("modified or untracked", result.stderr)

    def test_untracked_file_denied(self):
        (self.repo / "untracked.ores").write_text("throw 1;\n")
        result = self.run_verifier(self.sha)
        self.assertEqual(2, result.returncode)
        self.assertIn("modified or untracked", result.stderr)

if __name__ == "__main__":
    unittest.main()
