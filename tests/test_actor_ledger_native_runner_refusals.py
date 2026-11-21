#!/usr/bin/env python3
"""Source-free synthetic Git regression for the local actor acceptance runner."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = (pathlib.Path(__file__).resolve().parents[1] /
          "scripts/ci/run_actor_ledger_native_exact_head.sh")
GIT_ENV = (
    "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT",
)


class NativeRunnerRefusals(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.repo = pathlib.Path(directory.name)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        for key, value in (("user.name", "Source-free Fixture"),
                           ("user.email", "local@example.invalid")):
            subprocess.run(["git", "-C", str(self.repo), "config", key, value], check=True)
        self.target = self.repo / "scripts/ci/run_actor_ledger_native_exact_head.sh"
        self.target.parent.mkdir(parents=True)
        shutil.copy2(SCRIPT, self.target)
        self.target.chmod(0o755)
        subprocess.run(["git", "-C", str(self.repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-qm", "synthetic"], check=True)
        self.sha = subprocess.check_output(
            ["git", "-C", str(self.repo), "rev-parse", "HEAD"], text=True
        ).strip()

    def invoke(self, *args, extra_env=None):
        env = os.environ.copy()
        for name in GIT_ENV:
            env.pop(name, None)
        env.update(extra_env or {})
        return subprocess.run(["bash", str(self.target), *args],
                              cwd=self.repo, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=15)

    def test_missing_sha(self):
        self.assertEqual(self.invoke().returncode, 64)

    def test_malformed_sha(self):
        self.assertEqual(self.invoke("abc").returncode, 64)

    def test_wrong_head(self):
        self.assertEqual(self.invoke("0" * 40).returncode, 65)

    def test_dirty_checkout(self):
        (self.repo / "untracked.tmp").write_text("dirty", encoding="utf-8")
        self.assertEqual(self.invoke(self.sha).returncode, 65)

    def test_missing_compiler_input_is_not_a_pass(self):
        result = self.invoke(self.sha)
        self.assertEqual(result.returncode, 65, result.stderr)
        self.assertIn("missing/symlink input", result.stderr)

    def test_git_dir_override(self):
        self.assert_unsafe_git_env_denied("GIT_DIR", "/tmp/forged-repository")

    def test_git_object_override(self):
        self.assert_unsafe_git_env_denied("GIT_ALTERNATE_OBJECT_DIRECTORIES",
                                          "/tmp/forged-objects")

    def test_git_config_override(self):
        self.assert_unsafe_git_env_denied("GIT_CONFIG_COUNT", "1")

    def assert_unsafe_git_env_denied(self, key, value):
        result = self.invoke(self.sha, extra_env={key: value})
        self.assertEqual(result.returncode, 65)
        self.assertIn("unsafe caller Git environment", result.stderr)

    def test_runner_and_python_audit_are_attested(self):
        content = self.target.read_text(encoding="utf-8")
        self.assertIn("  scripts/ci/run_actor_ledger_native_exact_head.sh\n", content)
        self.assertIn("PYTHONOPTIMIZE=0 PYTHONNOUSERSITE=1 python3 -I -B", content)


if __name__ == "__main__":
    unittest.main(verbosity=2)
