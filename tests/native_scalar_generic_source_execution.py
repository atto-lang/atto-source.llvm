#!/usr/bin/env python3
"""Actual native Oreslang -> LLVM IR -> Clang guest i64 generic conformance.

Stage 1 intentionally admits only generics whose concrete representation is
int64. This test never treats unsupported classes/actors as generic support.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

VALID = {
    "explicit-scalar": ("""fnc identity<T>(T value): T { return value; }
pub fnc main(): int { return identity<int>(42); }
""", 42),
    "inferred-scalar": ("""fnc identity<T>(T value): T { return value; }
pub fnc main(): int { return identity(42); }
""", 42),
    "dependent-default": ("""fnc add<A, B=int, C=B>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<int>(19, 23); }
""", 42),
    "named-override": ("""fnc add<A, B=int, C=B>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<C=int, A=int>(19, 23); }
""", 42),
    "inferred-explicit-hole": ("""fnc identity<T=int>(T value): T { return value; }
pub fnc main(): int { return identity<T=_>(42); }
""", 42),
    "default-omission": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed(); }
""", 42),
}
INVALID = {
    "missing-required": ("""fnc fixed<T>(): int { return 42; }
pub fnc main(): int { return fixed(); }
""", "cannot infer generic parameter"),
    "uninferable-hole": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<_>(); }
""", "cannot infer generic parameter"),
    "nested-nonscalar": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Array<int>>(); }
""", "native scalar generic type requires int"),
    "unknown-type": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<bool>(); }
""", "native scalar generic type requires int"),
    "duplicate-named": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<T=int, T=int>(); }
""", "too many generic type arguments"),
    "unknown-named": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Other=int>(); }
""", "unknown named generic argument"),
    "positional-after-named": ("""fnc add<A, B=int>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<B=int, int>(19, 23); }
""", "positional generic argument after named argument"),
    "forward-default": ("""fnc invalid<A=B, B=int>(): int { return 42; }
pub fnc main(): int { return invalid<int>(); }
""", "forward or self-referential generic default"),
    "generic-main": ("""pub fnc main<T=int>(): int { return 42; }
""", "native entrypoint main cannot be generic"),
    "bad-nested-hole": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Option<_>>(); }
""", "native scalar generic type requires int"),
}

def run(argv):
    return subprocess.run(argv, capture_output=True, timeout=65, check=False)

def verify(compiler, clang):
    with tempfile.TemporaryDirectory(prefix="oreslang-native-generics-") as tmp:
        tmp = Path(tmp)
        for name, (source, exit_code) in VALID.items():
            path = tmp / (name + ".ores")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode or not result.stdout.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={result.returncode}, stderr={result.stderr!r}")
            ll = tmp / (name + ".ll")
            exe = tmp / name
            ll.write_bytes(result.stdout)
            linking = run([str(clang), "-Wno-override-module", "-x", "ir", str(ll), "-o", str(exe)])
            if linking.returncode:
                raise AssertionError(f"{name}: Clang failed: {linking.stderr!r}")
            executed = run([str(exe)])
            if executed.returncode != exit_code:
                raise AssertionError(f"{name}: guest exit {executed.returncode}, expected {exit_code}, stderr={executed.stderr!r}")
            print(f"PASS guest {name}: {exit_code}", flush=True)
        for name, (source, diagnostic) in INVALID.items():
            path = tmp / (name + ".ores")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode == 0 or result.returncode < 0 or result.stdout:
                raise AssertionError(f"{name}: compiler must reject before IR, rc={result.returncode}")
            if diagnostic.encode() not in result.stderr:
                raise AssertionError(f"{name}: expected {diagnostic!r}, got {result.stderr!r}")
            print(f"PASS reject {name}: {diagnostic}", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    verify(args.compiler.resolve(), args.clang.resolve())
