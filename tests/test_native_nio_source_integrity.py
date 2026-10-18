"""Source-free adversarial Git fixtures for native NIO worktree admission."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[1] / 'scripts' / 'verify-native-nio-source-local.py'


def run(*args, cwd, env=None):
    return subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True, check=False)


class NativeNioWorktreeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='native-nio-worktree-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / 'repo'
        self.repo.mkdir()
        (self.repo / 'scripts').mkdir()
        shutil.copyfile(CHECKER, self.repo / 'scripts' / CHECKER.name)
        (self.repo / '.gitignore').write_text('/build/\n/cmake-build-*/\n/.cache/\n*.o\n')
        (self.repo / 'source.c').write_text('int source(void) { return 1; }\n')
        for args in [('init', '-q'), ('config', 'user.name', 'Source Free Gate'),
                     ('config', 'user.email', 'fixture@example.invalid'),
                     ('add', '.'), ('commit', '-qm', 'initial fixture')]:
            out = run('git', *args, cwd=self.repo)
            self.assertEqual(0, out.returncode, out.stderr)
        self.head = run('git', 'rev-parse', 'HEAD', cwd=self.repo).stdout.strip()
        self.tree = run('git', 'rev-parse', 'HEAD^{tree}', cwd=self.repo).stdout.strip()

    def gate(self, *, commit=None, tree=None, env=None):
        return run(sys.executable, 'scripts/' + CHECKER.name,
                   commit or self.head, tree or self.tree, cwd=self.repo, env=env)

    def reject(self, snippet):
        out = self.gate()
        self.assertEqual(2, out.returncode, out.stderr)
        self.assertIn(snippet, out.stderr)

    def test_clean(self):
        self.assertEqual(0, self.gate().returncode)

    def test_allowed_ignored_build_outputs(self):
        for folder in ('build', 'cmake-build-debug', '.cache'):
            dest = self.repo / folder
            dest.mkdir()
            (dest / 'generated.o').write_bytes(b'object')
        self.assertEqual(0, self.gate().returncode)

    def test_commit_tree_mismatch(self):
        self.assertEqual(2, self.gate(commit='0' * 40).returncode)
        self.assertEqual(2, self.gate(tree='0' * 40).returncode)

    def test_dirty_tracked(self):
        (self.repo / 'source.c').write_text('changed\n')
        self.reject('dirty')

    def test_untracked_source(self):
        (self.repo / 'shadow.c').write_text('int x;\n')
        self.reject('dirty')

    def test_filemode_false_does_not_hide_chmod(self):
        self.assertEqual(0, run('git', 'config', 'core.fileMode', 'false', cwd=self.repo).returncode)
        (self.repo / 'source.c').chmod(0o755)
        ordinary = run('git', 'status', '--porcelain', cwd=self.repo)
        self.assertEqual('', ordinary.stdout)
        self.reject('file-mode-modified')

    def test_assume_unchanged(self):
        self.assertEqual(0, run('git', 'update-index', '--assume-unchanged', 'source.c', cwd=self.repo).returncode)
        (self.repo / 'source.c').write_text('hidden change\n')
        self.reject('hidden/sparse')

    def test_skip_worktree(self):
        self.assertEqual(0, run('git', 'update-index', '--skip-worktree', 'source.c', cwd=self.repo).returncode)
        (self.repo / 'source.c').write_text('hidden change\n')
        self.reject('hidden/sparse')

    def test_ignored_source_and_config(self):
        (self.repo / '.git' / 'info' / 'exclude').write_text('hidden.h\n')
        (self.repo / 'hidden.h').write_text('#define BYPASS 1\n')
        self.reject('ignored non-build')

    def test_ignored_object_outside_build(self):
        (self.repo / 'source.o').write_bytes(b'object')
        self.reject('ignored non-build')

    def test_clean_filter_cannot_hide_raw_source_edit(self):
        # Local, untracked .git/info/attributes and filter config are outside
        # worktree status, yet can cause clean-filtered *changed bytes* to be
        # reported as identical to HEAD. Compiler reads the unfiltered file.
        attrs = self.repo / '.git' / 'info' / 'attributes'
        attrs.write_text('source.c filter=mask\n')
        out = run('git', 'config', 'filter.mask.clean', 'tr A-Z a-z', cwd=self.repo)
        self.assertEqual(0, out.returncode, out.stderr)
        before = (self.repo / 'source.c').read_text()
        self.assertIn('int source', before)
        (self.repo / 'source.c').write_text(before.replace('int source', 'INT source'))
        # Prove this is a real Git-status blind spot, not an ordinary dirty edit.
        status = run('git', '-c', 'core.fileMode=true', '-c',
                     'core.fsmonitor=false', 'status', '--porcelain=v1',
                     cwd=self.repo)
        self.assertEqual('', status.stdout, status.stderr)
        self.reject('raw tracked blob mismatch')

    def test_git_index_override(self):
        env = os.environ.copy()
        env['GIT_INDEX_FILE'] = str(self.repo / '.git' / 'fake-index')
        out = self.gate(env=env)
        self.assertEqual(2, out.returncode)
        self.assertIn('Git checkout overrides', out.stderr)

    def test_postflight_mutation(self):
        self.assertEqual(0, self.gate().returncode)
        (self.repo / 'source.c').write_text('mutated during compile\n')
        self.reject('dirty')

    def test_build_symlink(self):
        outside = Path(self.tmp.name) / 'outside'
        outside.mkdir()
        (self.repo / 'build').symlink_to(outside, target_is_directory=True)
        self.reject('symlink')


if __name__ == '__main__':
    unittest.main()
