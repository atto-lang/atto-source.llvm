#!/usr/bin/env python3
"""Source -> LLVM -> native executable and fail-closed local class move matrix."""
import argparse
from pathlib import Path
import subprocess
import tempfile


PAIR = (
    "define class Pair as\n"
    "  pub let int a = 13;\n"
    "  pub let int b = 29;\n"
    "end\n"
)
OTHER = (
    "define class Other as\n"
    "  pub let int a = 10;\n"
    "end\n"
)


def invoke(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=75)


def require(condition, *details):
    """Assertions must remain effective under python -O in CI."""
    if not condition:
        raise AssertionError(details)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--sanitize", action="store_true")
    opts = parser.parse_args()

    successes = {
        "independent-owner-survives-move": PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair independent = new Pair();
  let mut Pair dest = rt take(source);
  return dest.a + independent.b;
}
""",
        "partial-field-move-keeps-sibling": PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  const moved: int = rt take source.a;
  return moved + source.b;
}
""",
        "move-parenthesized": PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  return dest.a + dest.b;
}
""",
        "move-unparenthesized": PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take source;
  return dest.a + dest.b;
}
""",
        "multi-hop-move": PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair second = rt take(source);
  let mut Pair third = rt take(second);
  return third.a + third.b;
}
""",
        "borrow-after-move": PAIR + """
fnc plus_one(borrow mut Pair pair): void {
  pair.b = 30;
  return;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  plus_one(rt borrow mut dest);
  return dest.a + dest.b - 1;
}
""",
        "guarded-move-taken": PAIR + """
pub fnc main(): int {
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a == 13) { return second.b + 13; } fi
  return second.a + second.b;
}
""",
        "guarded-move-fallthrough": PAIR + """
pub fnc main(): int {
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a eq 0) { return 0; } fi
  return second.a + second.b;
}
""",
        "guarded-scalar-branch-move": PAIR + """
pub fnc main(): int {
  const x: int = 42;
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a eq 13) { return rt take x; } fi
  return x;
}
""",
    }
    rejections = {
        "whole-move-after-partial-field-take": (PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  const moved: int = rt take source.a;
  let Pair dest = rt take(source);
  return dest.b + moved;
}
""", "ownership violation"),
        "borrow-after-whole-move": (PAIR + """
fnc view(borrow Pair candidate): int {
  return candidate.a;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  let Pair dest = rt take(source);
  return view(rt borrow source) + dest.b;
}
""", "ownership violation"),
        "guard-read-after-whole-move": (PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let Pair dest = rt take(source);
  if (source.a eq 13) { return 0; } fi
  return dest.a + dest.b;
}
""", "ownership violation"),
        "use-after-move": (PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  return source.a + dest.b;
}
""", "ownership violation: use-after-move"),
        "double-move": (PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  let mut Pair again = rt take(source);
  return dest.a + again.b;
}
""", "ownership violation: class move requires live unborrowed owner"),
        "wrong-type": (PAIR + OTHER + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let Other dest = rt take(source);
  return dest.a;
}
""", "ownership violation: class move type mismatch"),
        "borrowed-source": (PAIR + """
fnc steal(borrow Pair source): int {
  let mut Pair dest = rt take(source);
  return dest.a;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  return steal(rt borrow source);
}
""", "ownership violation: class move requires live unborrowed owner"),
        "unknown-owner": (PAIR + """
pub fnc main(): int {
  let mut Pair dest = rt take(not_defined);
  return dest.a;
}
""", "unknown class move source"),
        "implicit-copy": (PAIR + """
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = source;
  return dest.a;
}
""", "expected 'rt'"),
    }

    with tempfile.TemporaryDirectory(prefix="oreslang-class-moves-") as directory:
        root = Path(directory)
        for name, source in successes.items():
            path = root / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            compiled = invoke([opts.compiler, str(path)])
            require(compiled.returncode == 0 and bool(compiled.stdout),
                    name, compiled.returncode, compiled.stderr)
            require("gc.statepoint" not in compiled.stdout and
                    "gc.root" not in compiled.stdout,
                    name, "unexpected GC IR")
            ll = root / (name + ".ll")
            ll.write_text(compiled.stdout, encoding="utf-8")
            executable = root / name
            clang_args = [opts.clang, "-O1", "-x", "ir", str(ll), "-o", str(executable)]
            if opts.sanitize:
                clang_args[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            built = invoke(clang_args)
            require(built.returncode == 0, name, built.stderr)
            ran = invoke([str(executable)])
            require(ran.returncode == 42, name, ran.returncode, ran.stderr)
        for name, (source, diagnostic) in rejections.items():
            path = root / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            rejected = invoke([opts.compiler, str(path)])
            require(rejected.returncode != 0 and not rejected.stdout,
                    name, "invalid source emitted partial LLVM IR",
                    rejected.returncode, rejected.stdout, rejected.stderr)
            require(diagnostic in rejected.stderr,
                    name, diagnostic, rejected.stderr)
    print(f"PASS: {len(successes)} native class moves, {len(rejections)} no-IR rejections"
          + (" with ASan/UBSan" if opts.sanitize else ""))


if __name__ == "__main__":
    main()
