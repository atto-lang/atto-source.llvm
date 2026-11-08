#!/usr/bin/env python3
"""Synthetic CTest plumbing proof ONLY; never proof of an Oreslang compiled guest."""
from pathlib import Path
import json
import subprocess
import tempfile

from native_rt_scalar_certify import certify, GateFailed, TESTS, TARGETS


def fixture(root: Path, *, tamper: bool):
    source = root / 'synthetic_source'
    source.mkdir()
    shim = root / 'llvm-config'
    shim.write_text('#!/bin/sh\ncase "$1" in --version) echo 17.0.0 ;; '
                    '--cmakedir) echo "' + str(root) + '" ;; *) exit 2 ;; esac\n')
    shim.chmod(0o755)
    fake_clang = root / 'clang'
    fake_clang.write_text('#!/bin/sh\nif [ \"$1\" = \"-dumpversion\" ]; then echo 17.0.0; else exit 2; fi\n')
    fake_clang.chmod(0o755)
    (source / 'noop.c').write_text('int main(void) { return 0; }\n')
    cmake = ['cmake_minimum_required(VERSION 3.20)',
             'project(synthetic_proof LANGUAGES C)', 'enable_testing()']
    for target in TARGETS:
        cmake.append(f'add_executable({target} noop.c)')
    for index, name in enumerate(TESTS):
        command = ('${CMAKE_COMMAND} -E touch "' + str(source / 'malicious_untracked') + '"'
                   if tamper and index == 0 else '${CMAKE_COMMAND} -E true')
        cmake.append(f'add_test(NAME {name} COMMAND {command})')
    (source / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()
    git('init', '-q')
    git('config', 'user.name', 'Synthetic Verification')
    git('config', 'user.email', 'synthetic@example.invalid')
    git('add', '.')
    git('commit', '-qm', 'synthetic fixture')
    return source, git('rev-parse', 'HEAD'), str(shim), str(fake_clang)


def main():
    # Only the receipt and test-discovery mechanism is under test. The
    # compilers are deliberately fake C executables, never real LLVM output.
    with tempfile.TemporaryDirectory(prefix='synthetic-exact-head-') as td:
        root = Path(td) / 'positive'
        root.mkdir()
        source, head, llvm_config, clang = fixture(root, tamper=False)
        result = certify(source, head, llvm_config, clang, root / 'proof')
        if (result['status'] != 'EXACT_HEAD_EXECUTED_PASS' or
                result['tests']['executed_cases'] != len(TESTS) or
                result.get('post_build_source') != result['source'] or
                len(result.get('evidence_sha256', {})) != 5):
            raise AssertionError('synthetic certification mechanics unexpectedly failed')

        root = Path(td) / 'tampered'
        root.mkdir()
        source, head, llvm_config, clang = fixture(root, tamper=True)
        try:
            certify(source, head, llvm_config, clang, root / 'proof')
        except GateFailed:
            receipt = json.loads((root / 'proof' / 'receipt.json').read_text())
            if receipt['status'] != 'FAILED_OR_NOT_EXECUTED':
                raise AssertionError('tampered checkout incorrectly received PASS')
        else:
            raise AssertionError('checkout changed during CTest but was certified')

        print('SYNTHETIC_CTEST_PROVENANCE_MECHANICS_PASS')


if __name__ == '__main__':
    main()
