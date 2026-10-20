"""Source-free adversarial Git fixtures for native NIO worktree admission."""
from __future__ import annotations

import os
import shlex
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[1] / 'scripts' / 'verify-native-nio-source-local.py'
SANITIZER = Path(__file__).resolve().parents[1] / 'scripts' / 'test-native-vm-nio-host-sanitizers.sh'


def run(*args, cwd, env=None):
    return subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True, check=False)


class NativeNioWorktreeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='native-nio-worktree-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / 'repo'
        self.repo.mkdir()
        (self.repo / 'scripts').mkdir()
        # Fixture checker bytes come from the committed Git object, not from
        # a path which could be swapped immediately after the parent gate.
        committed_checker = subprocess.check_output(
            ('git', 'show', 'HEAD:scripts/verify-native-nio-source-local.py'),
            cwd=CHECKER.parents[1])
        (self.repo / 'scripts' / CHECKER.name).write_bytes(committed_checker)
        # Source-free stand-in: only proves the Git-blob-to-Bash loader,
        # never pretends to exercise native Oreslang source/runtime.
        (self.repo / 'scripts' / SANITIZER.name).write_text(
            '#!/usr/bin/env bash\n'
            'set -euo pipefail\n'
            'test "$ORES_NIO_REPO_ROOT" = "$PWD"\n'
            'echo SOURCE_FREE_PINNED_DRIVER_PASS\n')
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

    def gate_from_committed_shell(self, *, env=None):
        # Exercise the ACTUAL admission function against a source-free Git
        # fixture, without building the private C VM or LLVM runtime.
        shell = SANITIZER.read_text()
        marker = 'verify_exact_worktree() {\n'
        self.assertEqual(1, shell.count(marker))
        function = shell.split(marker, 1)[1].split('\n}\n', 1)[0]
        script = ('set -euo pipefail\n'
                  + 'head_sha=' + shlex.quote(self.head) + '\n'
                  + 'tree_sha=' + shlex.quote(self.tree) + '\n'
                  + marker + function + '\n}\nverify_exact_worktree\n')
        return run('bash', '-c', script, cwd=self.repo, env=env)

    def driver_from_committed_shell(self, *, env=None):
        # The committed VM workflow admits its Bash driver by three Git
        # identities before streaming the immutable object to Bash. The
        # synthetic repo contains no private runtime code or C++ compiler.
        script = '''set -euo pipefail
sanitizer=scripts/test-native-vm-nio-host-sanitizers.sh
approved="$(git rev-parse --verify "HEAD:$sanitizer")"
staged="$(git rev-parse --verify ":$sanitizer")"
raw="$(git hash-object --no-filters -- "$sanitizer")"
if [[ "$approved" != "$staged" || "$approved" != "$raw" ]]; then
  echo "NIO sanitizer driver differs from committed Git blob" >&2
  exit 2
fi
export ORES_NIO_REPO_ROOT="$GITHUB_WORKSPACE"
git cat-file blob "$approved" | bash -s
'''
        environment = (env or os.environ).copy()
        environment['GITHUB_WORKSPACE'] = str(self.repo)
        return run('bash', '-c', script, cwd=self.repo, env=environment)

    def test_vm_driver_executes_only_approved_git_blob(self):
        clean = self.driver_from_committed_shell()
        self.assertEqual(0, clean.returncode, clean.stderr)
        self.assertIn('SOURCE_FREE_PINNED_DRIVER_PASS', clean.stdout)
        driver = self.repo / 'scripts' / SANITIZER.name
        with driver.open('a') as out:
            out.write('\ntouch .git/should-not-execute\n')
        dirty = self.driver_from_committed_shell()
        self.assertEqual(2, dirty.returncode, dirty.stderr)
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())
        add = run('git', 'add', 'scripts/' + SANITIZER.name, cwd=self.repo)
        self.assertEqual(0, add.returncode, add.stderr)
        staged = self.driver_from_committed_shell()
        self.assertEqual(2, staged.returncode, staged.stderr)
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())

    def test_vm_driver_post_hash_path_swap_cannot_execute(self):
        # Fault injection *after* all hashes match, but before Bash starts.
        # A mutable script pathname would create the sentinel; a committed
        # blob remains trustworthy even when the worktree is raced.
        shim_dir = Path(self.tmp.name) / 'vm-driver-shim'
        shim_dir.mkdir()
        real_git = shutil.which('git')
        self.assertIsNotNone(real_git)
        shim = shim_dir / 'git'
        shim.write_text(
            '#!/usr/bin/env python3\n'
            'import os, subprocess, sys\n'
            'if sys.argv[1:3] == ["cat-file", "blob"]:\n'
            '    with open(os.environ["NIO_TEST_DRIVER_PATH"], "a") as out:\n'
            '        out.write(chr(10) + "touch .git/should-not-execute" + chr(10))\n'
            'raise SystemExit(subprocess.call([os.environ["NIO_TEST_REAL_GIT"], '
            '*sys.argv[1:]]))\n'
        )
        shim.chmod(0o755)
        env = os.environ.copy()
        env['NIO_TEST_DRIVER_PATH'] = str(self.repo / 'scripts' / SANITIZER.name)
        env['NIO_TEST_REAL_GIT'] = real_git
        env['PATH'] = str(shim_dir) + os.pathsep + env.get('PATH', '')
        result = self.driver_from_committed_shell(env=env)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn('SOURCE_FREE_PINNED_DRIVER_PASS', result.stdout)
        self.assertIn('should-not-execute',
                      (self.repo / 'scripts' / SANITIZER.name).read_text())
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())

    def test_github_workflows_require_immutable_blob_loader(self):
        # Read the *committed* workflow definitions. Future edits that
        # reintroduce a mutable verifier/test pathname must fail CI even
        # when all synthetic Git worktree fixtures would otherwise pass.
        root = CHECKER.parents[1]
        workflows = {
            '.github/workflows/native-nio-ownership-no-llvm.yml': (2, False),
            '.github/workflows/native-vm-nio-source-local-sanitizers.yml': (6, True),
        }
        for path, (expected_loaders, checks_test_blob) in workflows.items():
            with self.subTest(workflow=path):
                source = subprocess.check_output(
                    ('git', 'show', 'HEAD:' + path), cwd=root, text=True)
                self.assertGreaterEqual(
                    source.count('git cat-file blob "$approved"'),
                    expected_loaders)
                self.assertGreaterEqual(
                    source.count('git hash-object --no-filters -- "$checker"'),
                    expected_loaders - 2 * int(checks_test_blob))
                self.assertNotIn(
                    'python3 -I scripts/verify-native-nio-source-local.py',
                    source)
                self.assertNotIn(
                    'python3 -I -B scripts/verify-native-nio-source-local.py',
                    source)
                # The descriptor ownership workflow must perform its
                # immutable, raw-blob-verified postflight check AFTER all
                # executed test suites, not merely repeat two preflight
                # loaders that could leave compilation unchecked.
                if path.endswith('native-nio-ownership-no-llvm.yml'):
                    last_test = source.rfind(
                        'timeout 90s /tmp/oreslang-nio-fd-close-eintr-exact-head')
                    last_verifier = source.rfind(
                        'git cat-file blob "$approved"')
                    success_receipt = source.rfind(
                        'NATIVE_NIO_EXACT_SOURCE_PASS attempts=3')
                    self.assertGreaterEqual(last_test, 0)
                    self.assertGreater(last_verifier, last_test)
                    self.assertGreater(success_receipt, last_verifier)
                    self.assertIn(
                        'NIO postflight verifier differs from committed Git blob',
                        source)
                if checks_test_blob:
                    self.assertIn(
                        'git hash-object --no-filters -- "$sanitizer"', source)
                    self.assertIn(
                        'git cat-file blob "$approved" | bash -s', source)
                    self.assertNotIn(
                        'bash scripts/test-native-vm-nio-host-sanitizers.sh',
                        source)
                    self.assertIn(
                        'git hash-object --no-filters -- "$test_path"', source)
                    self.assertNotIn(
                        'python3 -I -B -m unittest discover', source)

    def test_clean_approved_blob_loader(self):
        result = self.gate_from_committed_shell()
        self.assertEqual(0, result.returncode, result.stderr)

    def test_mutated_verifier_is_not_executed(self):
        helper = self.repo / 'scripts' / CHECKER.name
        with helper.open('a') as out:
            out.write('\nopen(".git/should-not-execute", "w").write("pwned")\n')
        result = self.gate_from_committed_shell()
        self.assertNotEqual(0, result.returncode)
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())

    def test_staged_verifier_is_not_executed(self):
        helper = self.repo / 'scripts' / CHECKER.name
        with helper.open('a') as out:
            out.write('\nopen(".git/should-not-execute", "w").write("pwned")\n')
        add = run('git', 'add', 'scripts/' + CHECKER.name, cwd=self.repo)
        self.assertEqual(0, add.returncode, add.stderr)
        result = self.gate_from_committed_shell()
        self.assertNotEqual(0, result.returncode)
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())

    def test_post_hash_verifier_swap_cannot_execute_changed_python(self):
        # Swap the worktree verifier deterministically *after* raw hashing,
        # but just before Git provides the committed immutable blob.
        shim_dir = Path(self.tmp.name) / 'shim'
        shim_dir.mkdir()
        real_git = shutil.which('git')
        self.assertIsNotNone(real_git)
        shim = shim_dir / 'git'
        shim.write_text(
            '#!/usr/bin/env python3\n'
            'import os, subprocess, sys\n'
            'if sys.argv[1:3] == ["cat-file", "blob"]:\n'
            '    with open(os.environ["NIO_TEST_HELPER_PATH"], "a") as out:\n'
            '        out.write(chr(10) + "open(" + repr(".git/should-not-execute") '
            '+ ", " + repr("w") + ").write(" + repr("pwned") + ")" + chr(10))\n'
            'raise SystemExit(subprocess.call([os.environ["NIO_TEST_REAL_GIT"], '
            '*sys.argv[1:]]))\n'
        )
        shim.chmod(0o755)
        env = os.environ.copy()
        env['NIO_TEST_HELPER_PATH'] = str(self.repo / 'scripts' / CHECKER.name)
        env['NIO_TEST_REAL_GIT'] = real_git
        env['PATH'] = str(shim_dir) + os.pathsep + env.get('PATH', '')
        result = self.gate_from_committed_shell(env=env)
        self.assertEqual(2, result.returncode, result.stderr)
        self.assertIn('should-not-execute',
                      (self.repo / 'scripts' / CHECKER.name).read_text(),
                      'Git shim must actually swap the worktree helper')
        self.assertIn('raw tracked blob mismatch', result.stderr,
                      'a passing negative test must prove the committed checker ran')
        self.assertFalse((self.repo / '.git' / 'should-not-execute').exists())

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
        self.reject('raw tracked blob mismatch')

    def test_untracked_source(self):
        (self.repo / 'shadow.c').write_text('int x;\n')
        self.reject('dirty')

    def test_filemode_false_does_not_hide_chmod(self):
        self.assertEqual(0, run('git', 'config', 'core.fileMode', 'false', cwd=self.repo).returncode)
        (self.repo / 'source.c').chmod(0o755)
        ordinary = run('git', 'status', '--porcelain', cwd=self.repo)
        self.assertEqual('', ordinary.stdout)
        self.reject('tracked source executable mode mismatch')

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

    def test_filter_command_must_not_execute_during_integrity_check(self):
        attrs = self.repo / '.git' / 'info' / 'attributes'
        attrs.write_text('source.c filter=attack\n')
        command = "sh -c 'echo invoked > .git/clean-filter-invoked; tr A-Z a-z'"
        self.assertEqual(0, run('git', 'config', 'filter.attack.clean',
                                command, cwd=self.repo).returncode)
        before = (self.repo / 'source.c').read_text()
        (self.repo / 'source.c').write_text(before.replace('int source', 'INT source'))
        self.reject('raw tracked blob mismatch')
        self.assertFalse((self.repo / '.git' / 'clean-filter-invoked').exists(),
                         'the verifier must not execute local Git clean filters')

    def test_staged_index_must_equal_head(self):
        (self.repo / 'source.c').write_text('int source(void) { return 2; }\n')
        self.assertEqual(0, run('git', 'add', 'source.c', cwd=self.repo).returncode)
        self.reject('staged Git index differs from HEAD')

    def test_git_index_override(self):
        env = os.environ.copy()
        env['GIT_INDEX_FILE'] = str(self.repo / '.git' / 'fake-index')
        out = self.gate(env=env)
        self.assertEqual(2, out.returncode)
        self.assertIn('Git checkout overrides', out.stderr)

    def test_postflight_mutation(self):
        self.assertEqual(0, self.gate().returncode)
        (self.repo / 'source.c').write_text('mutated during compile\n')
        self.reject('raw tracked blob mismatch')

    def test_build_symlink(self):
        outside = Path(self.tmp.name) / 'outside'
        outside.mkdir()
        (self.repo / 'build').symlink_to(outside, target_is_directory=True)
        self.reject('symlink')


if __name__ == '__main__':
    unittest.main()
