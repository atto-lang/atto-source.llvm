#!/usr/bin/env python3
"""Real compiler -> LLVM IR -> Clang -> native executable named constructors.

The native constructor slice is stack-owned and scalar only. No generic
class specialization, reflection, default arguments or class escape is proved.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

BASE = """define class Pair as
  pub final int tens: first;
  pub final int ones: second;
  constructor(int first, int second) { return; }
end
"""
IMPLICIT = """define class Pair as
  pub val int tens;
  pub val int ones;
end
"""
POSITIVE = {
    "explicit-reordered": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(second: 2, first: 4);
  return p.tens * 10 + p.ones;
}
""", 42),
    "explicit-mixed": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(4, second: 2);
  return p.tens * 10 + p.ones;
}
""", 42),
    "explicit-positional": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(4, 2);
  return p.tens * 10 + p.ones;
}
""", 42),
    "implicit-reordered": (IMPLICIT + """pub fnc main(): int {
  let Pair p = new Pair(ones: 2, tens: 4);
  return p.tens * 10 + p.ones;
}
""", 42),
    "implicit-mixed": (IMPLICIT + """pub fnc main(): int {
  let Pair p = new Pair(4, ones: 2);
  return p.tens * 10 + p.ones;
}
""", 42),
    "val-owner-reordered": (BASE + """pub fnc main(): int {
  val Pair p = new Pair(second: 2, first: 4);
  return p.tens * 10 + p.ones;
}
""", 42),
    "let-mut-owner-reordered": (BASE + """pub fnc main(): int {
  let mut Pair p = new Pair(second: 2, first: 4);
  return p.tens * 10 + p.ones;
}
""", 42),
    # Name-to-slot substitution must survive both source-side arithmetic
    # and class-field initializer arithmetic, rather than merely reshuffle
    # constants at the LLVM ABI.
    "explicit-named-arithmetic": ("""define class Pair as
  pub final int left: first + 1;
  pub final int right: second * 2;
  constructor(int first, int second) { return; }
end
pub fnc main(): int {
  let Pair p = new Pair(second: 7 * 2, first: 10 + 3);
  return p.left + p.right;
}
""", 42),
    # The checked binder handles three independent formal slots, not only the
    # two-argument 'Pair' special case from the minimal native baseline.
    "triple-named-reordered": ("""define class Trio as
  pub final int a: first;
  pub final int b: second;
  pub final int c: third;
  constructor(int first, int second, int third) { return; }
end
pub fnc main(): int {
  let Trio t = new Trio(third: 3, first: 19, second: 20);
  return t.a + t.b + t.c;
}
""", 42),
    "triple-positional-named": ("""define class Trio as
  pub final int a: first;
  pub final int b: second;
  pub final int c: third;
  constructor(int first, int second, int third) { return; }
end
pub fnc main(): int {
  let Trio t = new Trio(19, third: 3, second: 20);
  return t.a + t.b + t.c;
}
""", 42),
}

NEGATIVE = {
    "unknown-label": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(first: 4, unknown: 2);
  return p.tens;
}
""", "unknown named native constructor argument 'unknown'"),
    "case-sensitive-label": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(First: 4, second: 2);
  return p.tens;
}
""", "unknown named native constructor argument 'First'"),
    "duplicate-label": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(first: 4, first: 2);
  return p.tens;
}
""", "duplicate named constructor argument 'first'"),
    "positional-after-named": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(second: 2, 4);
  return p.tens;
}
""", "positional constructor argument cannot follow named argument"),
    "positional-slot-collision": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(4, first: 2);
  return p.tens;
}
""", "duplicate native constructor argument binding"),
    "missing-argument": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(first: 4);
  return p.tens;
}
""", "native class constructor argument arity mismatch"),
    "excess-argument": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(first: 4, second: 2, extra: 9);
  return p.tens;
}
""", "native class constructor argument arity mismatch"),
    "wrong-formal-type": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(second: 2, first: true);
  return p.tens;
}
""", "native class constructor argument requires int, got bool"),
    "late-unknown-before-actual": (BASE + """pub fnc main(): int {
  let Pair p = new Pair(first: missing(), unknown: 2);
  return p.tens;
}
""", "unknown named native constructor argument 'unknown'"),
    "triple-positional-slot-collision": ("""define class Trio as
  pub final int a: first;
  pub final int b: second;
  pub final int c: third;
  constructor(int first, int second, int third) { return; }
end
pub fnc main(): int {
  let Trio t = new Trio(19, third: 3, first: 20);
  return t.a + t.b + t.c;
}
""", "duplicate native constructor argument binding"),
}
MAX_OUTPUT = 2 * 1024 * 1024


def execute(argv):
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        completed = subprocess.run(argv, stdout=stdout, stderr=stderr,
                                   check=False, timeout=65)
        stdout.seek(0, 2)
        stderr.seek(0, 2)
        if stdout.tell() > MAX_OUTPUT or stderr.tell() > MAX_OUTPUT:
            raise AssertionError("native constructor tool output exceeds bound")
        stdout.seek(0)
        stderr.seek(0)
        return completed.returncode, stdout.read(), stderr.read()


def verify(compiler, clang, optimization):
    if set(POSITIVE) & set(NEGATIVE):
        raise AssertionError("duplicate named constructor fixture ID")
    with tempfile.TemporaryDirectory(prefix="atto-native-named-constructor-") as root:
        work = Path(root)
        for name, (source, expected) in POSITIVE.items():
            src = work / (name + ".atto")
            src.write_text(source, encoding="utf-8")
            rc, ir, err = execute([str(compiler), str(src)])
            if rc or err or not ir.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={rc}, stderr={err!r}")
            ll = work / (name + ".ll")
            binary = work / name
            ll.write_bytes(ir)
            rc, out, err = execute([
                str(clang), optimization, "-Wno-override-module",
                "-x", "ir", str(ll), "-o", str(binary),
            ])
            if rc or out or err:
                raise AssertionError(
                    f"{name}: native linker rc={rc}, stdout={out!r}, stderr={err!r}")
            rc, out, err = execute([str(binary)])
            if rc != expected or out or err:
                raise AssertionError(
                    f"{name}: guest exit={rc} expected={expected}, "
                    f"stdout={out!r} stderr={err!r}")
            print(f"PASS constructor guest {name}: {expected}", flush=True)
        for name, (source, diagnostic) in NEGATIVE.items():
            src = work / (name + ".atto")
            src.write_text(source, encoding="utf-8")
            rc, out, err = execute([str(compiler), str(src)])
            if rc <= 0 or out:
                raise AssertionError(
                    f"{name}: compiler accepted invalid constructor or emitted IR")
            if any(token in err for token in (
                    b"AddressSanitizer", b"UndefinedBehaviorSanitizer",
                    b"LeakSanitizer")):
                raise AssertionError(f"{name}: sanitizer failure: {err!r}")
            if diagnostic.encode("utf-8") not in err:
                raise AssertionError(
                    f"{name}: expected {diagnostic!r}, got {err!r}")
            print(f"PASS constructor reject {name}: {diagnostic}", flush=True)


if __name__ == "__main__":
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--compiler", type=Path, required=True)
    arguments.add_argument("--clang", type=Path, required=True)
    arguments.add_argument("--clang-opt", choices=("-O0", "-O2"), default="-O0")
    args = arguments.parse_args()
    verify(args.compiler.resolve(), args.clang.resolve(), args.clang_opt)
