#!/usr/bin/env python3
"""Source-free, local-only Git ref deletion lease experiments.

This is NOT the LLVM private CI publisher and cannot authorize source export.
All Git transports in this test are isolated temporary local bare repos.
It exercises git push --force-with-lease=refs/heads/X:<expected> <remote> :refs/heads/X
for a deletion, including competing ref updates and a hook-triggered race.
"""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest


REF = "refs/heads/ci/atomic-ref-rollback-test"
SHA = re.compile(r"[0-9a-f]{40}\Z")


class LocalGitAtomicRefCleanupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ores-source-free-cas-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.remote = self.root / "remote.git"
        self.work = self.root / "work"
        self.git("init", "--bare", "--quiet", str(self.remote))
        self.git("init", "--quiet", str(self.work))
        self.git("-C", str(self.work), "config", "user.name", "Synthetic CI Test")
        self.git("-C", str(self.work), "config", "user.email", "ci-test@example.invalid")
        self.a = self.commit("A")
        self.git("-C", str(self.work), "push", "--quiet", str(self.remote), f"HEAD:{REF}")

    def git(self, *args, check=True):
        # Even the supplied remotes are test-created local filesystem paths.
        command = ["git", "-c", "protocol.file.allow=always", *args]
        proc = subprocess.run(command, cwd=self.root, text=True,
                              capture_output=True, timeout=15, check=False,
                              env={**os.environ, "GIT_CONFIG_NOSYSTEM": "1",
                                   "GIT_TERMINAL_PROMPT": "0"})
        if check and proc.returncode != 0:
            raise AssertionError(f"git command failed: {args!r}: {proc.stderr}")
        return proc

    def commit(self, label):
        self.git("-C", str(self.work), "commit", "-q", "--allow-empty", "-m", label)
        sha = self.git("-C", str(self.work), "rev-parse", "HEAD").stdout.strip()
        self.assertRegex(sha, SHA)
        return sha

    def read(self):
        proc = self.git("--git-dir", str(self.remote), "rev-parse", "--verify", REF, check=False)
        return proc.stdout.strip() if proc.returncode == 0 else None

    def set_ref(self, sha):
        self.git("--git-dir", str(self.remote), "update-ref", REF, sha)

    def delete_with_lease(self, expected):
        self.assertRegex(expected, SHA)
        return self.git("-C", str(self.work), "push", "--porcelain",
                        f"--force-with-lease={REF}:{expected}",
                        str(self.remote), f":{REF}", check=False)

    def test_exact_sha_ref_is_deleted(self):
        self.assertEqual(self.read(), self.a)
        done = self.delete_with_lease(self.a)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIsNone(self.read())

    def test_stale_expected_sha_never_deletes_competitor(self):
        b = self.commit("B")
        self.git("-C", str(self.work), "push", "--quiet", str(self.remote), f"HEAD:{REF}")
        self.assertEqual(self.read(), b)
        self.assertNotEqual(b, self.a)
        result = self.delete_with_lease(self.a)
        self.assertNotEqual(result.returncode, 0, "stale deletion succeeded")
        self.assertEqual(self.read(), b, "competitor ref deleted")

    def test_competing_update_inside_receive_hook_does_not_get_deleted(self):
        b = self.commit("B")
        self.git("-C", str(self.work), "push", "--quiet", str(self.remote),
                 f"HEAD:refs/heads/ci/competitor-object")
        self.assertEqual(self.read(), self.a)
        hook = self.remote / "hooks" / "pre-receive"
        hook.write_text("#!/bin/sh\n"
                        f"git update-ref {REF} {b} {self.a} || exit 1\n"
                        "exit 0\n", encoding="utf-8")
        hook.chmod(0o755)
        result = self.delete_with_lease(self.a)
        self.assertNotEqual(result.returncode, 0,
                            "deletion succeeded despite concurrent move")
        self.assertEqual(self.read(), b, "concurrent writer's new ref deleted")

    def test_repeated_cleanup_fails_closed(self):
        self.assertEqual(self.delete_with_lease(self.a).returncode, 0)
        result = self.delete_with_lease(self.a)
        self.assertNotEqual(result.returncode, 0,
                            "unconfirmed repeat treated as success")
        self.assertIsNone(self.read())

    def test_remote_hook_rejection_preserves_ref(self):
        hook = self.remote / "hooks" / "pre-receive"
        hook.write_text("#!/bin/sh\necho refused >&2\nexit 1\n", encoding="utf-8")
        hook.chmod(0o755)
        result = self.delete_with_lease(self.a)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.read(), self.a)

    def test_sha_lease_cannot_detect_aba_and_is_not_authorization(self):
        b = self.commit("B")
        self.git("-C", str(self.work), "push", "--quiet", str(self.remote),
                 f"HEAD:refs/heads/ci/competitor-object")
        self.set_ref(b)
        self.set_ref(self.a)  # A->B->A; current SHA matches but history changed
        self.assertEqual(self.read(), self.a)
        result = self.delete_with_lease(self.a)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(self.read())
        # This intentional success proves a SHA lease alone is insufficient
        # for safe source-bearing cleanup without exclusive namespace owner
        # and immutable pre-existing authorization.


if __name__ == "__main__":
    unittest.main(verbosity=2)
