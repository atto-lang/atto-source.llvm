#!/usr/bin/env python3
"""Real compiled Oreslang String → verified LLVM IR → native C runtime."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

POSITIVE = (
    ('literal', 'pub fnc main(): int { val word: String = "snow"; return len(word); }', 4),
    ('clone_move', '''pub fnc main(): int {
       val first: String = "ping";
       val duplicate: String = rt copy first;
       val moved: String = rt take duplicate;
       return len(first) + len(moved);
    }''', 8),
    ('unicode', 'pub fnc main(): int { val word: String = "é🚀"; return len(word); }', 6),
    ('embedded_nul', r'pub fnc main(): int { val word: String = "x\0y"; return len(word); }', 3),
    ('empty', 'pub fnc main(): int { val word: String = ""; return len(word); }', 0),
)
NEGATIVE = (
    ('use_after_move', '''pub fnc main(): int {
        val a: String = "x"; val b: String = rt take a;
        return len(a);
    }''', 'length reads moved'),
    ('double_take', '''pub fnc main(): int {
        val a: String = "x"; val b: String = rt take a;
        val c: String = rt take a; return len(c);
    }''', 'use of moved'),
    ('duplicate', '''pub fnc main(): int {
        val a: String = "x"; val a: String = "y"; return len(a);
    }''', 'duplicate String binding'),
    ('borrow_escape', '''pub fnc main(): int {
        val a: String = "x"; return a;
    }''', "expected 'len'"),
    ('unsupported_async', '''pub async fnc main(): int {
        val a: String = "x"; return len(a);
    }''', "expected 'fnc'"),
    ('unsupported_call', '''pub fnc main(): int {
        val a: String = request(); return len(a);
    }''', "expected 'rt'"),
    ('invalid_escape', r'''pub fnc main(): int {
        val a: String = "\q"; return len(a);
    }''', 'unsupported String escape'),
)

def compile_source(compiler, source_bytes, working):
    name = working / 'test.ores'
    name.write_bytes(source_bytes)
    return subprocess.run(
        [compiler, '--experimental-string', str(name)],
        capture_output=True, timeout=30, check=False,
    )

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--native-runtime', required=True)
    p.add_argument('--include-root', required=True)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='oreslang-string-execute-') as directory:
        cwd = Path(directory)
        for case, text, expected in POSITIVE:
            compiled = compile_source(args.compiler, text.encode('utf-8'), cwd)
            assert compiled.returncode == 0, (case, compiled.stderr.decode('utf-8', 'replace'))
            ir = compiled.stdout.decode('utf-8')
            assert ir.startswith('; ModuleID'), (case, ir[:250])
            assert 'call i32 @oreslang_string_new_utf8' in ir or case == 'empty', case
            assert 'call void @oreslang_string_drop' in ir, case
            if case == 'clone_move':
                assert 'call i32 @oreslang_string_clone' in ir, ir
            native = cwd / f'{case}-binary'
            link = subprocess.run(
                [args.clang, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-x', 'ir', '-', '-x', 'c', args.native_runtime,
                 '-std=c11', '-I', args.include_root, '-o', str(native)],
                input=compiled.stdout, capture_output=True, timeout=60, check=False,
            )
            assert link.returncode == 0, (case, link.stderr.decode('utf-8', 'replace'))
            env = dict(os.environ)
            env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
            run = subprocess.run([str(native)], capture_output=True, timeout=15,
                                 env=env, check=False)
            assert run.returncode == expected, (case, run.returncode,
                                                 run.stderr.decode('utf-8', 'replace'))
            assert b'ERROR: AddressSanitizer' not in run.stderr, (case, run.stderr)
            print('PASS native String', case)
        for case, text, message in NEGATIVE:
            compiled = compile_source(args.compiler, text.encode('utf-8'), cwd)
            assert compiled.returncode != 0, (case, 'invalid String source compiled')
            assert not compiled.stdout, (case, 'rejected source emitted LLVM IR')
            assert message.encode() in compiled.stderr, (case, compiled.stderr)
            print('PASS rejected String', case)
        # Invalid UTF-8 is a runtime error; it must release all earlier owners.
        invalid = (b'pub fnc main(): int { val first: String = "ok"; '
                   b'val bad: String = "\xc0\xaf"; return len(first); }')
        compiled = compile_source(args.compiler, invalid, cwd)
        assert compiled.returncode == 0, compiled.stderr
        native = cwd / 'invalid-utf8-binary'
        link = subprocess.run(
            [args.clang, '-fsanitize=address,undefined', '-x', 'ir', '-',
             '-x', 'c', args.native_runtime, '-std=c11',
             '-I', args.include_root, '-o', str(native)],
            input=compiled.stdout, capture_output=True, timeout=60, check=False,
        )
        assert link.returncode == 0, link.stderr
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
        run = subprocess.run([str(native)], capture_output=True, timeout=15,
                             env=env, check=False)
        assert run.returncode == 93, (run.returncode, run.stderr)
        print('PASS failed UTF-8 construction cleans prior owners')

if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f'experimental String test failed: {error}', file=sys.stderr)
        raise SystemExit(1)
