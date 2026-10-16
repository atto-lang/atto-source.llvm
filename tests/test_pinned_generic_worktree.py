"""Adversarial tests for the pinned common-source worktree."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from pinned_generic_worktree import attest_pinned_worktree


def git(root, *args):
    cp = subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                        text=True, check=True)
    return cp.stdout.strip()


class ExactCommonSourceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="test-pinned-ores-common-")
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        git(self.repo, "init", "-q")
        for path, data in {
            "scripts/verify_generic_guest_parity.py": "print('guest')\n",
            "contracts/v1/generic-scalar/verify.py": "print('canonical')\n",
            "contracts/v1/generic-scalar/fixtures/explicit.ores": "pub fnc main(): int { return 42; }\n",
            ".gitignore": "evil.py\n__pycache__/\n",
        }.items():
            f = self.repo / path
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(data)
        git(self.repo, "add", ".")
        subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=CI",
                        "-c", "user.email=ci@example.invalid", "commit", "-qm", "seed"], check=True)
        self.sha = git(self.repo, "rev-parse", "HEAD^{tree}")

    def attest(self):
        attest_pinned_worktree(self.repo, self.repo, self.sha)

    def test_clean_exact_tree_passes(self):
        self.attest()

    def test_assume_unchanged_bypass_rejected(self):
        target = "contracts/v1/generic-scalar/verify.py"
        git(self.repo, "update-index", "--assume-unchanged", target)
        (self.repo / target).write_text("print('spoofed verification')\n")
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.attest()

    def test_skip_worktree_bypass_rejected(self):
        target = "scripts/verify_generic_guest_parity.py"
        git(self.repo, "update-index", "--skip-worktree", target)
        (self.repo / target).write_text("print('spoofed execution')\n")
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.attest()

    def test_ignored_module_shadow_rejected(self):
        (self.repo / "evil.py").write_text("print('unexpected module')\n")
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        with self.assertRaisesRegex(ValueError, "extra source file"):
            self.attest()

    def test_symlinked_tracked_file_rejected(self):
        original = self.repo / "contracts/v1/generic-scalar/verify.py"
        target = self.repo / "scripts/verify_generic_guest_parity.py"
        original.unlink()
        original.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "symlinked"):
            self.attest()

    def test_untracked_nonignored_file_rejected(self):
        (self.repo / "surprise.py").write_text("pass\n")
        with self.assertRaisesRegex(ValueError, "extra source file"):
            self.attest()

    def test_tracked_file_removed_rejected(self):
        (self.repo / "scripts/verify_generic_guest_parity.py").unlink()
        with self.assertRaisesRegex(ValueError, "missing pinned"):
            self.attest()

    def test_executable_bit_change_rejected(self):
        path = self.repo / "scripts/verify_generic_guest_parity.py"
        path.chmod(0o755)
        with self.assertRaisesRegex(ValueError, "executable mode"):
            self.attest()

    def test_wrong_tree_rejected(self):
        with self.assertRaises(ValueError):
            attest_pinned_worktree(self.repo, self.repo, "0" * 40)

    def test_carrier_nested_checkout_passes(self):
        nested = self.repo / "common"
        nested.mkdir()
        (self.repo / "scripts").rename(nested / "scripts")
        (self.repo / "contracts").rename(nested / "contracts")
        (self.repo / ".gitignore").rename(nested / ".gitignore")
        git(self.repo, "add", "-A")
        subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=CI",
                        "-c", "user.email=ci@example.invalid", "commit", "-qm", "carrier"], check=True)
        tree = git(self.repo, "rev-parse", "HEAD:common")
        attest_pinned_worktree(self.repo, nested, tree)


if __name__ == "__main__":
    unittest.main()
