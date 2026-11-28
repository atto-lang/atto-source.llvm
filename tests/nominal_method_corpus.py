#!/usr/bin/env python3
"""Run pinned shared nominal fixtures through native LLVM CLI and Clang.

Local trusted toolchains only; digests bind files, not build provenance.
Exit 0: all cases pass; 1: failure; 2: unsupported coverage remains.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

CORPUS_BLOB = '9fa4f22c0b1ea9468aad2657cfff7b5b02869cde'
DIAGNOSTICS = {
    'OBSOLETE_METHOD_INTRODUCER': b'obsolete method introducer',
    'DUPLICATE_INSTANCE_METHOD': b'duplicate native method name/arity',
    'MISSING_INSTANCE_OBLIGATION': b'MISSING_INSTANCE_OBLIGATION',
}
# Only explicitly identified compiler-owned unsupported capabilities qualify.
UNSUPPORTED = {
    'interface': (b'NATIVE_CLASS_INHERITANCE_UNSUPPORTED',
                  b'native trait/interface forward requires a verified type-level linker; unsupported'),
    'trait': (b'NATIVE_CLASS_INHERITANCE_UNSUPPORTED',
              b'native trait/interface forward requires a verified type-level linker; unsupported'),
}


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(argv, cwd):
    return subprocess.run(argv, cwd=cwd, capture_output=True, timeout=45, check=False)


def rejected(case, result):
    if (result.returncode != 1 or result.stdout or
            not result.stderr.startswith(b'oreslang-llvmc: ')):
        return 'FAIL'
    expected = DIAGNOSTICS.get(case.get('diagnostic_family'))
    if case['expect'] != 'compile_and_run' and expected and expected in result.stderr:
        return 'PASS_DIAGNOSTIC'
    if any(marker in result.stderr for marker in UNSUPPORTED.get(case['id'].split('.')[0], ())):
        return 'UNSUPPORTED_PROFILE'
    return 'FAIL'


def verify_case(case, compiler, clang, sanitize):
    with tempfile.TemporaryDirectory(prefix='atto-llvm-nominal-') as tmp:
        root = Path(tmp)
        source = root / 'main.atto'
        source.write_bytes(case['source'].encode('utf-8'))
        row = {'id': case['id'], 'source_sha256': sha256(source), 'status': 'FAIL'}
        try:
            result = run([str(compiler), str(source)], root)
            row['compiler_exit'] = result.returncode
            if result.returncode != 0:
                row['status'] = rejected(case, result)
                row['diagnostic'] = result.stderr.decode('utf-8', errors='replace')[:4096]
                return row
            # Negative cases must be rejected by the frontend, never the linker.
            if case['expect'] != 'compile_and_run' or not result.stdout or result.stderr:
                return row
            ir = root / 'main.ll'
            ir.write_bytes(result.stdout)
            row['ir_sha256'] = sha256(ir)
            guest = root / 'guest'
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else []
            linked = run([str(clang), *flags, '-O1', str(ir), '-o', str(guest)], root)
            row['link_exit'] = linked.returncode
            if linked.returncode != 0 or not guest.is_file():
                row['diagnostic'] = linked.stderr.decode('utf-8', errors='replace')[:4096]
                return row
            executed = run([str(guest)], root)
            row['guest_exit'] = executed.returncode
            if (executed.returncode == case['expected_return_int'] and
                    not executed.stdout and not executed.stderr):
                row['status'] = 'PASS_GUEST'
        except (OSError, subprocess.TimeoutExpired) as error:
            row['diagnostic'] = str(error)
        return row


def verify(manifest, compiler, compiler_sha, clang, clang_sha, sanitize=False):
    data = manifest.read_bytes()
    if hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest() != CORPUS_BLOB:
        raise ValueError('wrong corpus Git blob')
    if sha256(compiler) != compiler_sha or sha256(clang) != clang_sha:
        raise ValueError('toolchain digest mismatch')
    corpus = json.loads(data)
    rows = [verify_case(case, compiler, clang, sanitize) for case in corpus['cases']]
    if sha256(compiler) != compiler_sha or sha256(clang) != clang_sha:
        raise ValueError('toolchain digest changed during verification')
    return {'profile': 'native-llvm', 'corpus_git_blob': CORPUS_BLOB,
            'compiler_sha256': compiler_sha, 'clang_sha256': clang_sha,
            'sanitized': sanitize, 'cases': rows,
            'counts': dict(collections.Counter(row['status'] for row in rows))}


def exit_status(report):
    counts = report['counts']
    if len(report['cases']) != 120 or counts.get('FAIL', 0):
        return 1
    return 2 if counts.get('UNSUPPORTED_PROFILE', 0) else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--compiler-sha256', required=True)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--clang-sha256', required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    report = verify(args.manifest, args.compiler.resolve(strict=True), args.compiler_sha256,
                    args.clang.resolve(strict=True), args.clang_sha256, args.sanitize)
    print(json.dumps(report, indent=2))
    return exit_status(report)


if __name__ == '__main__':
    raise SystemExit(main())
