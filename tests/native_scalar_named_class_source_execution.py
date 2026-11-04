#!/usr/bin/env python3
"""Compiler -> LLVM IR -> Clang -> native guest and fail-closed diagnostics.

First scalar-only native named nominal class specialization: no String/Array
generic layouts, generic class methods, defaulted values or reflection.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

POSITIVES = {
    "named-reordered-type": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", 42),
    "named-constructor-value": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int, A=int> p = new Pair<A=int, B=int>(second: 23, first: 19);\n  return p.first + p.second;\n}\n", 42),
    "positional-named-prefix": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<int, B=int> p = new Pair<B=int, A=int>(19, 23);\n  return p.first + p.second;\n}\n", 42),
    "mutable-generic-owner": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let mut Pair<B=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", 42),
    "const-generic-owner": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  const Pair<B=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", 42),
    "class-generic-default": ("define class Box<T=int> as\n  pub val T value;\nend\npub fnc main(): int {\n  let Box a = new Box(42);\n  return a.value;\n}\n", 42),
    "class-generic-dependent-default": ("define class Pair<A, B=A> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<A=int> p = new Pair<B=int, A=int>(19, 23);\n  return p.first + p.second;\n}\n", 42),
    "explicit-constructor-generic": ("define class Pair<A, B> as\n  pub final int first: x;\n  pub final int second: y;\n  constructor(A x, B y) { return; }\nend\npub fnc main(): int {\n  let Pair<B=int, A=int> p = new Pair<A=int, B=int>(y: 23, x: 19);\n  return p.first + p.second;\n}\n", 42),
}
NEGATIVES = {
    "unknown-declared": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<X=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "unknown named native class generic 'X'"),
    "unknown-constructed": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int, A=int> p = new Pair<A=int, X=int>(19, 23);\n  return p.first + p.second;\n}\n", "unknown named native class generic 'X'"),
    "duplicate-named": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<A=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "duplicate native class generic argument"),
    "positional-collision": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "duplicate native class generic argument"),
    "positional-after-named": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int, int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "positional class generic argument after named argument"),
    "missing-required": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "cannot infer native class generic parameter 'A'"),
    "excess": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<A=int, B=int, C=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "too many native class generic arguments"),
    "case-sensitive": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<b=int, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "unknown named native class generic 'b'"),
    "missing-constructor-specialization": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=int, A=int> p = new Pair(19, 23);\n  return p.first + p.second;\n}\n", "cannot infer native class generic parameter"),
    "nonscalar-bool": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=bool, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "native generic class specialization supports only int"),
    "nonscalar-string": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=String, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "native generic class specialization supports only int"),
    "hole-in-generic": ("define class Pair<A, B> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int {\n  let Pair<B=_, A=int> p = new Pair<A=int, B=int>(19, 23);\n  return p.first + p.second;\n}\n", "native class generic inference holes require typed constructor inference"),
    "non-generic-application": ("define class Simple as\n  pub val int value;\nend\npub fnc main(): int {\n  let Simple<T=int> p = new Simple<T=int>(42);\n  return p.value;\n}\n", "non-generic native class cannot accept type arguments"),
    "duplicate-declaration": ("define class Pair<A, A> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int { return 42; }\n", "duplicate native class generic parameter"),
    "forward-default": ("define class Pair<A=B, B=int> as\n  pub val A first;\n  pub val B second;\nend\npub fnc main(): int { return 42; }\n", "forward or self-referential native class generic default"),
    "generic-method-unsupported": ("define class Box<T=int> as\n  pub val T value;\n  pub get(): int { return 42; }\nend\npub fnc main(): int { return 42; }\n", "native generic class methods require typed receiver monomorphization"),
}
MAX_OUTPUT = 2 * 1024 * 1024

def command(argv):
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        proc = subprocess.run(argv, stdout=output, stderr=errors,
                              timeout=65, check=False)
        output.seek(0, 2)
        errors.seek(0, 2)
        if output.tell() > MAX_OUTPUT or errors.tell() > MAX_OUTPUT:
            raise AssertionError("native generic-class output exceeds bound")
        output.seek(0)
        errors.seek(0)
        return proc.returncode, output.read(), errors.read()

def verify(compiler, clang, opt):
    if set(POSITIVES) & set(NEGATIVES):
        raise AssertionError("duplicate scalar generic-class fixture ID")
    with tempfile.TemporaryDirectory(prefix="atto-native-named-class-") as td:
        root = Path(td)
        for name, (source, expected) in POSITIVES.items():
            src = root / (name + ".atto")
            src.write_text(source, encoding="utf-8")
            rc, ir, err = command([str(compiler), str(src)])
            if rc or err or not ir.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={rc}, stderr={err!r}")
            ll = root / (name + ".ll")
            binary = root / name
            ll.write_bytes(ir)
            rc, out, err = command([
                str(clang), opt, "-Wno-override-module", "-x", "ir",
                str(ll), "-o", str(binary),
            ])
            if rc or out or err:
                raise AssertionError(
                    f"{name}: Clang rc={rc}, stdout={out!r}, stderr={err!r}")
            rc, out, err = command([str(binary)])
            if rc != expected or out or err:
                raise AssertionError(
                    f"{name}: guest exit={rc} expected={expected}, output={out!r}, err={err!r}")
            print(f"PASS named generic class guest {name}: {expected}", flush=True)
        for name, (source, diagnostic) in NEGATIVES.items():
            src = root / (name + ".atto")
            src.write_text(source, encoding="utf-8")
            rc, out, err = command([str(compiler), str(src)])
            if rc != 1 or out:
                raise AssertionError(
                    f"{name}: compiler must reject with exit 1 and no IR, got rc={rc}")
            if any(x in err for x in (
                b"AddressSanitizer", b"UndefinedBehaviorSanitizer",
                b"LeakSanitizer")):
                raise AssertionError(f"{name}: sanitizer failure: {err!r}")
            if diagnostic.encode() not in err:
                raise AssertionError(
                    f"{name}: expected {diagnostic!r}, got {err!r}")
            print(f"PASS reject named generic class {name}", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--clang-opt", choices=("-O0", "-O2"), default="-O0")
    args = parser.parse_args()
    verify(args.compiler.resolve(), args.clang.resolve(), args.clang_opt)
