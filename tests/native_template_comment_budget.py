#!/usr/bin/env python3
"""Compiler-backed quote/template comment resource limit test.

This executes the actual oreslang-llvmc frontend, not an independent lexer.
A positive case may still fail later lowering if String guest support is
unavailable; never report such a case as guest execution/parity.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def run(compiler: pathlib.Path, path: pathlib.Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(compiler), str(path)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )


def source(depth: int) -> str:
    return (
        'pub fnc main(): int {\n'
        '  const quote = `prefix ${' +
        '/*' * depth + ' nested comment ' + '*/' * depth +
        '"ok"}`;\n'
        '  return 42;\n'
        '}\n'
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError('usage: native_template_comment_budget.py <oreslang-llvmc>')
    compiler = pathlib.Path(sys.argv[1]).resolve()
    if not compiler.is_file():
        raise RuntimeError('missing native compiler')
    with tempfile.TemporaryDirectory(prefix='oreslang-template-comment-') as tmp:
        path = pathlib.Path(tmp) / 'comments.atto'
        control = pathlib.Path(tmp) / 'control.atto'
        control.write_text('pub fnc main(): int {return 42;}\n', encoding='utf-8')
        baseline = run(compiler, control)
        if baseline.returncode != 0 or not baseline.stdout:
            raise RuntimeError('scalar native compiler control failed: ' +
                               repr(baseline.stderr[:512]))
        for count in (1, 128):
            path.write_text(source(count), encoding='utf-8')
            result = run(compiler, path)
            if b'block comment nesting limit exceeded (128)' in result.stderr:
                raise RuntimeError('valid comment depth rejected: '+str(count))
            # Executed compiler must actually emit LLVM IR for each valid input.
            # This is compilation evidence only; no guest executable is run here.
            if result.returncode != 0 or b'define i64 @__oreslang_guest_main(' not in result.stdout:
                raise RuntimeError('valid template did not compile at depth ' +
                                   str(count) + ': ' + repr(result.stderr[:512]))
        # A line comment in interpolation ends on CR, LF, or CRLF.
        # Require actual LLVM IR, not merely a scanner acceptance oracle.
        for label, newline in (('lf', '\n'), ('cr', '\r'), ('crlf', '\r\n')):
            path.write_text(
                'pub fnc main(): int {\n'
                '  const quote = `prefix ${// ignored }' + newline + '"ok"} suffix`;\n'
                '  return 42;\n'
                '}\n',
                encoding='utf-8',
            )
            compiled = run(compiler, path)
            if compiled.returncode != 0 or b'define i64 @__oreslang_guest_main(' not in compiled.stdout:
                raise RuntimeError('native template line-comment ' + label +
                                   ' did not compile: ' + repr(compiled.stderr[:512]))
        path.write_text(source(129), encoding='utf-8')
        rejected = run(compiler, path)
        if (rejected.returncode == 0 or rejected.stdout or
                b'block comment nesting limit exceeded (128)' not in rejected.stderr):
            raise RuntimeError('129 nested template comments escaped the native frontend')
    print('native LLVM frontend: template comment 1/128 admitted; 129 denied')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print('FAIL native template comment budget: ' + str(error), file=sys.stderr)
        sys.exit(1)
