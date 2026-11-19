#!/usr/bin/env python3
"""Negative admission checks; do NOT execute/attest compiled AttoLang guests."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CMAKE_DIR = ROOT / 'tests/tenant-vm'
WRAPPER = ROOT / 'tools/tenant-vm/verify-atto-e2e.sh'


class RequiredGuestGateTests(unittest.TestCase):
    def cmake_refusal(self, args, expected):
        with tempfile.TemporaryDirectory() as work:
            result = subprocess.run(
                ['cmake', '-S', str(CMAKE_DIR), '-B', work,
                 '-DTENANT_VM_REQUIRE_ATTO_GUESTS=ON', *args],
                text=True, capture_output=True, timeout=45)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(expected, result.stdout + result.stderr)

    def test_missing_compiler_is_not_host_only_success(self):
        self.cmake_refusal(
            ['-DTENANT_VM_ATTO_COMPILER='],
            'ATTO_E2E_NOT_EXECUTED: explicit executable TENANT_VM_ATTO_COMPILER required')

    def test_nonexistent_compiler_is_rejected(self):
        self.cmake_refusal(
            ['-DTENANT_VM_ATTO_COMPILER=/nonexistent/atto-compiler'],
            'ATTO_E2E_NOT_EXECUTED: explicit executable TENANT_VM_ATTO_COMPILER required')

    def test_precompiled_llvm_ir_is_not_accepted_as_source_compilation(self):
        with tempfile.TemporaryDirectory() as tmp:
            compiler = pathlib.Path(tmp) / 'fake-compiler'
            compiler.write_text('#!/bin/sh\nexit 0\n')
            compiler.chmod(0o755)
            self.cmake_refusal(
                [f'-DTENANT_VM_ATTO_COMPILER={compiler}',
                 f'-DTENANT_VM_ATTO_IR_DIR={tmp}'],
                'ATTO_E2E_NOT_EXECUTED: pre-generated .ll is not a source compiler run')

    def test_wrapper_refuses_unpinned_head_before_compiling(self):
        env = dict(os.environ)
        env.pop('ATTO_E2E_EXPECTED_HEAD_SHA', None)
        r = subprocess.run(['bash', str(WRAPPER)], cwd=ROOT,
                           capture_output=True, text=True, env=env, timeout=15)
        self.assertEqual(r.returncode, 2)
        self.assertIn('ATTO_E2E_NOT_EXECUTED', r.stderr)

    def test_wrapper_refuses_invalid_head_before_compiling(self):
        env = dict(os.environ, ATTO_E2E_EXPECTED_HEAD_SHA='0'*40)
        r = subprocess.run(['bash', str(WRAPPER)], cwd=ROOT,
                           capture_output=True, text=True, env=env, timeout=15)
        self.assertEqual(r.returncode, 2)
        self.assertIn('exact source head mismatch', r.stderr)


if __name__ == '__main__':
    unittest.main()
