#!/usr/bin/env python3
"""Untrusted checkout and missing test proof: fail-closed preflight tests."""
import contextlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from native_rt_scalar_certify import GateFailed, verify_checkout, verify_junit, major, verify_toolchain, command


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True, stderr=subprocess.PIPE).strip()


class CheckoutGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='exact-head-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'source'
        self.root.mkdir()
        git(self.root, 'init', '-q')
        git(self.root, 'config', 'user.email', 'verification@example.invalid')
        git(self.root, 'config', 'user.name', 'Verification Fixture')
        (self.root/'source.cpp').write_bytes(b'int main() { return 0; }\n')
        (self.root/'script.sh').write_bytes(b'#!/bin/sh\nexit 0\n')
        (self.root/'script.sh').chmod(0o755)
        (self.root/'shortcut').symlink_to('source.cpp')
        git(self.root, 'add', '.')
        git(self.root, 'commit', '-qm', 'test fixture')
        self.head = git(self.root, 'rev-parse', 'HEAD')

    def test_full_tree_blob_provenance_pass(self):
        result=verify_checkout(self.root, self.head)
        self.assertEqual(result['head'], self.head)
        self.assertEqual(result['verified_blobs'], 3)
        self.assertTrue(result['clean_worktree'])

    def test_wrong_head_fails(self):
        with self.assertRaisesRegex(GateFailed, 'HEAD mismatch'):
            verify_checkout(self.root, '0'*40)

    def test_untracked_file_fails(self):
        (self.root/'untracked').write_text('x')
        with self.assertRaisesRegex(GateFailed, 'dirty'):
            verify_checkout(self.root, self.head)

    def test_modified_tracked_file_fails(self):
        (self.root/'source.cpp').write_text('changed')
        with self.assertRaisesRegex(GateFailed, 'dirty'):
            verify_checkout(self.root, self.head)

    def test_mode_change_fails(self):
        (self.root/'script.sh').chmod(0o644)
        with self.assertRaises(GateFailed):
            verify_checkout(self.root, self.head)

    def test_symlink_change_fails(self):
        (self.root/'shortcut').unlink()
        (self.root/'shortcut').symlink_to('/tmp/missing')
        with self.assertRaises(GateFailed):
            verify_checkout(self.root, self.head)

    def test_assume_unchanged_does_not_bypass_sha_verification(self):
        git(self.root,'update-index','--assume-unchanged','source.cpp')
        (self.root/'source.cpp').write_text('changed')
        with self.assertRaisesRegex(GateFailed, 'worktree blob mismatch'):
            verify_checkout(self.root,self.head)

    def test_nested_repository_root_not_accepted(self):
        (self.root/'nested').mkdir()
        with self.assertRaisesRegex(GateFailed, 'checkout root'):
            verify_checkout(self.root/'nested',self.head)

    def test_git_submodule_entry_fails_closed(self):
        git(self.root,'update-index','--add','--cacheinfo','160000,' + '1'*40 + ',foreign')
        git(self.root,'commit','-qm','record untrusted submodule')
        head=git(self.root,'rev-parse','HEAD')
        with self.assertRaises(GateFailed):
            verify_checkout(self.root,head)  # dirty or unsupported submodule; fail closed


class TestReportTests(unittest.TestCase):
    def test_major_strict_parsing(self):
        self.assertEqual(major('19.1.2'),19)
        with self.assertRaises(GateFailed):
            major('Ubuntu clang version 19')
        with self.assertRaises(GateFailed):
            major('17.2\nextra')

    def test_junit_missing_and_skipped_fail(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'x.xml'
            with self.assertRaisesRegex(GateFailed,'missing'):
                verify_junit(path)
            path.write_text('<testsuite><testcase name="one"><skipped /></testcase></testsuite>')
            with self.assertRaisesRegex(GateFailed,'missing, extra, skipped'):
                verify_junit(path)

    def test_junit_duplicate_fails(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'x.xml'
            path.write_text('<testsuite><testcase name="same"/><testcase name="same"/></testsuite>')
            with self.assertRaisesRegex(GateFailed,'duplicate'):
                verify_junit(path)

    def test_child_python_does_not_mutate_source_with_pycache(self):
        with tempfile.TemporaryDirectory() as t:
            module = Path(t) / 'private_module.py'
            module.write_text('VALUE = 42\n')
            command([sys.executable, '-c', 'import private_module; assert private_module.VALUE == 42'], cwd=t)
            self.assertFalse((Path(t) / '__pycache__').exists())

    def test_version_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)
            llvm=p/'llvm-config'
            clang=p/'clang'
            llvm.write_text('#!/bin/sh\nif [ "$1" = "--version" ]; then echo 19.1.7; elif [ "$1" = "--cmakedir" ]; then echo /tmp; fi\n')
            clang.write_text('#!/bin/sh\necho 17.0.0\n')
            llvm.chmod(0o755)
            clang.chmod(0o755)
            with self.assertRaisesRegex(GateFailed,'mismatches|!='):
                verify_toolchain(str(llvm),str(clang))

if __name__=='__main__':
    unittest.main(verbosity=2)
