#!/usr/bin/env python3
"""Source -> LLVM codegen -> ORC execution, with fail-closed admission checks.

This is a real compiled-source execution gate; it does NOT imply Truffle parity,
automatic tiering, sandboxing, actor JIT support, or Java differential execution.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(compiler, source, *flags):
    return subprocess.run(
        [str(compiler), *flags, str(source)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=20,
    )


def require(condition, message, outcome=None):
    if not condition:
        raise RuntimeError(message + (": " + repr(outcome) if outcome else ""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    clang = args.clang.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="oreslang-orc-source-") as tmp:
        source = Path(tmp) / "main.ores"
        native_ir = Path(tmp) / "main.ll"
        native_exe = Path(tmp) / "main-native"

        # AOT and ORC share parse, type/borrow checking, LLVM codegen, and
        # the generated i32 native wrapper's lossless 0..255 exit policy.
        positives = [
            ("basic", "pub fnc main():int { return 42; }", 42),
            ("forward-call", """pub fnc main():int { return calculate(6, 7); }
fnc calculate(int x, int y):int { return x * y; }""", 42),
            ("nested-call", """fnc twice(int x):int { return x * 2; }
pub fnc main():int { return twice(twice(10)) + 2; }""", 42),
            ("negative", "fnc main():int { return -1; }", 1),
            ("oversize", "fnc main():int { return 256; }", 1),
            ("max-code", "fnc main():int { return 255; }", 255),
            ("void", "fnc main():void { }", 0),
        ]
        for name, text, status in positives:
            source.write_text(text + "\n", encoding="utf-8")
            normal = run(compiler, source)
            require(normal.returncode == 0 and b"define i32 @main(" in normal.stdout,
                    "compiler did not produce native wrapper for " + name, normal)
            # The same emitted LLVM IR must compile and execute through Clang.
            # Match *observed process behavior* rather than IR text alone.
            native_ir.write_bytes(normal.stdout)
            linked = subprocess.run(
                [str(clang), "-O0", "-x", "ir", str(native_ir), "-o", str(native_exe)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False, timeout=20)
            require(linked.returncode == 0 and native_exe.is_file(),
                    "Clang AOT link failed for " + name, linked)
            native = subprocess.run(
                [str(native_exe)], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False, timeout=20)
            require(native.returncode == status and not native.stdout and not native.stderr,
                    "Clang AOT outcome mismatch for " + name, native)
            for iteration in range(2):
                executed = run(compiler, source, "--jit")
                require(executed.returncode == status and not executed.stdout
                        and not executed.stderr,
                        "ORC exit mismatch for " + name + "/" + str(iteration), executed)

        # Execute existing checked-in source fixtures, not merely bespoke JIT
        # examples. Each fixture must agree with Clang AOT and LLVM ORC.
        fixtures = Path(__file__).resolve().parent / "fixtures"
        fixture_cases = [
            ("forward-call-main.ores", 42),
            ("native-borrow-read.ores", 83),
            ("native-reborrow-mut.ores", 40),
            ("native-name-arity-overloads.ores", 24),
            ("named-call-reordered-main.ores", 43),
            ("class-final-read-native.ores", 42),
        ]
        for fixture_name, expected in fixture_cases:
            fixture = fixtures / fixture_name
            require(fixture.is_file(), "missing checked-in fixture: " + fixture_name)
            compiled = run(compiler, fixture)
            require(compiled.returncode == 0 and b"define i32 @main(" in compiled.stdout,
                    "fixture failed source-to-IR: " + fixture_name, compiled)
            native_ir.write_bytes(compiled.stdout)
            linked = subprocess.run(
                [str(clang), "-O0", "-x", "ir", str(native_ir), "-o", str(native_exe)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False, timeout=20)
            require(linked.returncode == 0, "fixture failed Clang link: " + fixture_name,
                    linked)
            native = subprocess.run(
                [str(native_exe)], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False, timeout=20)
            require(native.returncode == expected and not native.stdout and not native.stderr,
                    "fixture failed Clang AOT: " + fixture_name, native)
            jit = run(compiler, fixture, "--jit")
            require(jit.returncode == expected and not jit.stdout and not jit.stderr,
                    "fixture ORC/AOT divergence: " + fixture_name, jit)

        source.write_text(positives[1][1] + "\n", encoding="utf-8")
        shaken = run(compiler, source, "--jit", "--tree-shake")
        require(shaken.returncode == 42 and not shaken.stdout and not shaken.stderr,
                "ORC tree-shake source execution diverged", shaken)

        # Do not execute code merely because ORC could accept its LLVM IR.
        negatives = [
            ("no-entry", "fnc helper():int { return 42; }", (), "requires native i32 main"),
            ("float-division", "fnc main():int { return 3 / 2; }", (), None),
            ("unknown-call", "fnc main():int { return missing(); }", (), None),
            ("cross-target", "fnc main():int { return 42; }",
             ("--target=wasm32",), "trusted host"),
            ("embedded-target", "fnc main():int { return 42; }",
             ("--target=thumbv7m-none-eabi",), "trusted host"),
            ("heap-experiment", "fnc main():int { return 42; }",
             ("--experimental-owned-int",), "trusted host"),
            ("rt", "fnc main():int { return 42; }",
             ("--rt-check",), "trusted host"),
            ("duplicate-jit", "fnc main():int { return 42; }",
             ("--jit",), "duplicate JIT"),
        ]
        for name, text, flags, diagnostic in negatives:
            source.write_text(text + "\n", encoding="utf-8")
            rejected = run(compiler, source, "--jit", *flags)
            require(rejected.returncode != 0 and not rejected.stdout and rejected.stderr,
                    "invalid JIT source or CLI was accepted: " + name, rejected)
            if diagnostic is not None:
                require(diagnostic.encode() in rejected.stderr,
                        "missing specific rejection diagnostic: " + name, rejected)
    print("ORC JIT 20 executions + 13 Clang AOT comparisons + tree-shake + 8 negative admissions PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("ORC JIT source gate FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
