#!/usr/bin/env python3
"""Source-free negative admission tests; never compile or transfer private code."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().with_name("run_pinned_compiled_actor_lifecycle.sh")
OTHER_HEAD = "f" * 40


class PinnedActorGuestRunnerNegatives(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "checkout"
        self.root.mkdir()
        for args in (("init", "-q"), ("config", "user.email", "ci@example.invalid"),
                     ("config", "user.name", "CI")):
            subprocess.run(("git", "-C", str(self.root), *args), check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (self.root / "README.md").write_text("source-free synthetic repo\n")
        subprocess.run(("git", "-C", str(self.root), "add", "README.md"), check=True)
        subprocess.run(("git", "-C", str(self.root), "commit", "-qm", "synthetic"), check=True)
        self.head = subprocess.check_output(
            ("git", "-C", str(self.root), "rev-parse", "HEAD"), text=True).strip()

    def attempt(self, head=None, cwd=None, args=()):
        env = dict(os.environ)
        if head is None:
            env.pop("EXPECTED_HEAD", None)
        else:
            env["EXPECTED_HEAD"] = head
        return subprocess.run(("bash", str(SCRIPT), *args), cwd=cwd or self.root,
                              env=env, capture_output=True, text=True, timeout=15)

    def reject(self, phrase, **kw):
        result = self.attempt(**kw)
        self.assertNotEqual(result.returncode, 0, "invalid admission passed")
        self.assertIn("NOT_EXECUTED:", result.stderr)
        self.assertIn(phrase, result.stderr)
        self.assertNotIn("SOURCE_LOCAL_COMPILED_ACTOR_LIFECYCLE_AND_FULL_CTEST_PASS",
                         result.stdout)

    def test_script_is_valid_bash(self):
        subprocess.run(("bash", "-n", str(SCRIPT)), check=True, timeout=10)

    def test_missing_reviewed_commit_rejected(self):
        self.reject("EXPECTED_HEAD must", head=None)

    def test_forged_reviewed_commit_rejected(self):
        self.reject("checkout differs", head=OTHER_HEAD)

    def test_short_commit_rejected(self):
        self.reject("EXPECTED_HEAD must", head="abcdef")

    def test_untracked_source_rejected(self):
        (self.root / "untracked.ores").write_text("fake")
        self.reject("untracked checkout", head=self.head)

    def test_modified_tracked_source_rejected(self):
        (self.root / "README.md").write_text("mutated")
        self.reject("tracked source checkout modified", head=self.head)

    def test_preexisting_build_dir_rejected(self):
        (self.root / "build").mkdir()
        self.reject("no preexisting build directory", head=self.head)

    def test_preexisting_build_symlink_rejected(self):
        (self.root / "build").symlink_to(self.root)
        self.reject("no preexisting build directory", head=self.head)

    def test_subdirectory_execution_rejected(self):
        (self.root / "subdir").mkdir()
        self.reject("checkout root", head=self.head, cwd=self.root / "subdir")

    def test_positional_args_rejected(self):
        self.reject("no positional flags", head=self.head, args=("--skip-guest",))


if __name__ == "__main__":
    unittest.main()
