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
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="oreslang-orc-source-") as tmp:
        source = Path(tmp) / "main.ores"

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
            for iteration in range(2):
                executed = run(compiler, source, "--jit")
                require(executed.returncode == status and not executed.stdout
                        and not executed.stderr,
                        "ORC exit mismatch for " + name + "/" + str(iteration), executed)

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
    print("ORC JIT compiled-source 7x2 host execution + 8 negative admission PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("ORC JIT source gate FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
