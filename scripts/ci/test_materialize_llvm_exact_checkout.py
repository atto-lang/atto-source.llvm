#!/usr/bin/env python3
"""Fully source-free test of Git sparse + trusted raw-blob materialization."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

VERIFY = Path(__file__).with_name('verify_llvm_exact_checkout.py')
MATERIALIZE = Path(__file__).with_name('materialize_llvm_exact_checkout.py')


def call(cwd, *argv, check=True, env=None):
    return subprocess.run(argv, cwd=cwd, check=check, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, env=env)


class SourceFreeSparseMaterialization(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.source = base / 'source'
        self.checkout = base / 'target'
        self.source.mkdir()
        call(self.source, 'git', 'init', '-q')
        call(self.source, 'git', 'config', 'user.name', 'Fixture')
        call(self.source, 'git', 'config', 'user.email', 'fixture@example.invalid')
        (self.source / 'nested').mkdir()
        (self.source / 'nested' / 'payload').write_bytes(b'original\x00bytes\n')
        (self.source / 'exe.sh').write_text('#!/bin/sh\nexit 0\n')
        (self.source / 'exe.sh').chmod(0o755)
        (self.source / '.gitattributes').write_text('* filter=tripwire\n')
        call(self.source, 'git', 'add', '.')
        call(self.source, 'git', 'commit', '-qm', 'original')
        self.sha = call(self.source, 'git', 'rev-parse', 'HEAD').stdout.strip()
        # --no-checkout avoids ordinary checkout smudge. Sparse set excludes
        # *all* tracked paths, even if a PR knows every filename in advance.
        call(base, 'git', 'clone', '-q', '--no-checkout', str(self.source), str(self.checkout))
        call(self.checkout, 'git', 'sparse-checkout', 'set', '--no-cone', '/*', '!/*')
        self.marker = base / 'TRIPWIRE_RAN'
        call(self.checkout, 'git', 'config', 'filter.tripwire.smudge',
             f'sh -c "touch {self.marker}; cat"')
        call(self.checkout, 'git', 'config', 'filter.tripwire.required', 'true')
        call(self.checkout, 'git', 'checkout', '-q', self.sha)
        self.assertFalse(self.marker.exists())
        self.assertEqual([p.name for p in self.checkout.iterdir()], ['.git'])

    def materialize(self, success=True, env=None):
        r = call(self.checkout, sys.executable, str(MATERIALIZE), self.sha, check=False, env=env)
        self.assertEqual(r.returncode == 0, success, r.stderr + r.stdout)

    def test_raw_bytes_no_smudge(self):
        self.materialize()
        self.assertFalse(self.marker.exists())
        self.assertEqual((self.checkout / 'nested' / 'payload').read_bytes(), b'original\x00bytes\n')
        self.assertTrue((self.checkout / 'exe.sh').stat().st_mode & 0o111)
        r = call(self.checkout, sys.executable, str(VERIFY), self.sha)
        self.assertIn('PASS', r.stdout)
        self.assertFalse(self.marker.exists())

    def test_actions_checkout_v4_non_cone_local_config(self):
        # Pinned checkout v4 sets core.sparseCheckout in LOCAL git config,
        # unlike `git sparse-checkout set`, which enables worktreeConfig.
        self.assertEqual(call(self.checkout, 'git', 'config', '--local', '--bool', '--get',
                              'extensions.worktreeConfig', check=False).returncode, 0)
        call(self.checkout, 'git', 'config', '--worktree', 'core.sparseCheckout', 'false')
        call(self.checkout, 'git', 'config', '--local', 'extensions.worktreeConfig', 'false')
        call(self.checkout, 'git', 'config', '--local', 'core.sparseCheckout', 'true')
        self.materialize()
        r=call(self.checkout, sys.executable, str(VERIFY), self.sha)
        self.assertIn('PASS', r.stdout)
        self.assertFalse(self.marker.exists())

    def test_reject_preexisting_untracked_file(self):
        (self.checkout / 'surprise').write_text('malicious')
        self.materialize(False)

    def test_reject_preexisting_symlink(self):
        (self.checkout / 'nested').symlink_to('/tmp', target_is_directory=True)
        self.materialize(False)

    def test_reject_wrong_commit(self):
        self.sha = 'b' * 40
        self.materialize(False)

    def test_reject_non_sparse(self):
        call(self.checkout, 'git', 'config', '--worktree', 'core.sparseCheckout', 'false')
        self.materialize(False)

    def test_reject_committed_symlink_before_write(self):
        (self.source / 'a-symlink').symlink_to('/etc/passwd')
        call(self.source, 'git', 'add', 'a-symlink')
        call(self.source, 'git', 'commit', '-qm', 'symlink')
        self.sha = call(self.source, 'git', 'rev-parse', 'HEAD').stdout.strip()
        # A new sparse fetch/checkout never materializes any source file.
        call(self.checkout, 'git', 'fetch', '-q', 'origin', self.sha)
        call(self.checkout, 'git', 'checkout', '-q', self.sha)
        self.materialize(False)
        self.assertEqual([p.name for p in self.checkout.iterdir()], ['.git'])

    def test_reject_injected_git_env_and_ignore_filter(self):
        e=os.environ.copy()
        e.update(GIT_INDEX_FILE=str(self.checkout / '.git' / 'bad-index'),
                 GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='filter.tripwire.smudge',
                 GIT_CONFIG_VALUE_0='sh -c "exit 99"')
        self.materialize(True, e)
        self.assertFalse(self.marker.exists())
        call(self.checkout, sys.executable, str(VERIFY), self.sha)


if __name__ == '__main__':
    unittest.main(verbosity=2)
