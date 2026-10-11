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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--sanitize", action="store_true")
    opts = parser.parse_args()

    successes = {
        "move-parenthesized": PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = rt take(source);
  return dest.a + dest.b;
}
""",
        "move-unparenthesized": PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = rt take source;
  return dest.a + dest.b;
}
""",
        "multi-hop-move": PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair second = rt take(source);
  let Pair third = rt take(second);
  return third.a + third.b;
}
""",
        "borrow-after-move": PAIR + """
fnc plus_one(borrow mut Pair pair): void {
  pair.b = 30;
  return;
}
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = rt take(source);
  plus_one(rt borrow mut dest);
  return dest.a + dest.b - 1;
}
""",
    }
    rejections = {
        "use-after-move": (PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = rt take(source);
  return source.a + dest.b;
}
""", "ownership violation: use-after-move"),
        "double-move": (PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = rt take(source);
  let Pair again = rt take(source);
  return dest.a + again.b;
}
""", "ownership violation: class move requires live unborrowed owner"),
        "wrong-type": (PAIR + OTHER + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Other dest = rt take(source);
  return dest.a;
}
""", "ownership violation: class move type mismatch"),
        "borrowed-source": (PAIR + """
fnc steal(borrow Pair source): int {
  let Pair dest = rt take(source);
  return dest.a;
}
pub fnc main(): int {
  let Pair source = new Pair();
  return steal(rt borrow source);
}
""", "ownership violation: class move requires live unborrowed owner"),
        "unknown-owner": (PAIR + """
pub fnc main(): int {
  let Pair dest = rt take(not_defined);
  return dest.a;
}
""", "unknown class move source"),
        "implicit-copy": (PAIR + """
pub fnc main(): int {
  let Pair source = new Pair();
  let Pair dest = source;
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
            assert compiled.returncode == 0 and compiled.stdout, (
                name, compiled.returncode, compiled.stderr)
            assert "gc.statepoint" not in compiled.stdout and "gc.root" not in compiled.stdout
            ll = root / (name + ".ll")
            ll.write_text(compiled.stdout, encoding="utf-8")
            executable = root / name
            clang_args = [opts.clang, "-O1", "-x", "ir", str(ll), "-o", str(executable)]
            if opts.sanitize:
                clang_args[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            built = invoke(clang_args)
            assert built.returncode == 0, (name, built.stderr)
            ran = invoke([str(executable)])
            assert ran.returncode == 42, (name, ran.returncode, ran.stderr)
        for name, (source, diagnostic) in rejections.items():
            path = root / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            rejected = invoke([opts.compiler, str(path)])
            assert rejected.returncode != 0 and not rejected.stdout, (
                name, "invalid source emitted partial LLVM IR", rejected.stdout)
            assert diagnostic in rejected.stderr, (name, diagnostic, rejected.stderr)
    print(f"PASS: {len(successes)} native class moves, {len(rejections)} no-IR rejections"
          + (" with ASan/UBSan" if opts.sanitize else ""))


if __name__ == "__main__":
    main()
