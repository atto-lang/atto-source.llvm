#!/usr/bin/env python3
"""Real LLVM-to-Clang branch-local scope proof and no-IR negative gates."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RECORD = (
    "define class Record as\n"
    "  pub let int score = 0;\n"
    "end\n"
    "fnc store(borrow mut Record r, int v): void {\n"
    "  r.score = v;\n"
    "  return;\n"
    "}\n"
)

GUESTS = {
    "both-branches": RECORD + """
fnc choose(borrow mut Record r, int n): void {
  if (n eq 0) {
    val left: int = 41;
    store(rt borrow mut r, left);
  } else {
    const right: int = 42;
    store(rt borrow mut r, right);
  } fi
  return;
}
pub fnc main(): int {
  let mut Record r = new Record();
  choose(rt borrow mut r, 0);
  choose(rt borrow mut r, 1);
  return r.score;
}
""",
    "sequential-scope-init": RECORD + """
pub fnc main(): int {
  let mut Record r = new Record();
  if (1 eq 1) {
    val a: int = 19;
    val b: int = a + 23;
    store(rt borrow mut r, b);
  } else {
    val other: int = 0;
    store(rt borrow mut r, other);
  } fi
  return r.score;
}
""",
    "same-name-disjoint-scopes": RECORD + """
pub fnc main(): int {
  let mut Record r = new Record();
  if (3 eq 4) {
    val v: int = 22;
    store(rt borrow mut r, v + 20);
  } else {
    val v: int = 12;
    store(rt borrow mut r, v + 30);
  } fi
  return r.score;
}
""",
    "defer-after-join": RECORD + """
fnc increment(borrow mut Record r): void {
  r.score = r.score + 1;
  return;
}
fnc action(borrow mut Record r, int n): void {
  defer increment(rt borrow mut r);
  if (n eq 0) {
    val x: int = 41;
    store(rt borrow mut r, x);
  } else {
    val y: int = 41;
    store(rt borrow mut r, y);
  } fi
  return;
}
pub fnc main(): int {
  let mut Record r = new Record();
  action(rt borrow mut r, 1);
  return r.score;
}
""",
    "scoped-borrow-loan": RECORD + """
fnc passthrough(borrow int n): int {
  return n;
}
pub fnc main(): int {
  let mut Record r = new Record();
  if (1 == 1) {
    val x: int = 42;
    store(rt borrow mut r, passthrough(rt borrow x));
  } else {
    store(rt borrow mut r, 1);
  } fi
  return r.score;
}
""",
}

BAD = {
    "escaped-scope": (RECORD + """
pub fnc main(): int {
  if (1 eq 1) { val leaked: int = 42; } else { } fi
  return leaked;
}
""", "unknown parameter 'leaked'"),
    "cross-arm-leak": (RECORD + """
pub fnc main(): int {
  let mut Record r = new Record();
  if (1 eq 0) { val x: int = 42; }
  else { store(rt borrow mut r, x); } fi
  return r.score;
}
""", "unknown parameter 'x'"),
    "self-initialization": ("""
pub fnc main(): int {
  if (1 eq 1) { val x: int = x + 1; } else { } fi
  return 42;
}
""", "unknown parameter 'x'"),
    "duplicate-in-arm": ("""
pub fnc main(): int {
  if (1 eq 1) {
    val x: int = 1;
    val x: int = 2;
  } else { } fi
  return 42;
}
""", "duplicate branch-local name 'x'"),
    "shadow-owner": ("""
fnc calculate(int x): int {
  if (1 eq 1) { val x: int = 99; } else { } fi
  return x;
}
pub fnc main(): int { return calculate(42); }
""", "duplicate branch-local name 'x'"),
    "nontrivial-class": (RECORD + """
pub fnc main(): int {
  if (1 eq 1) {
    let Record r = new Record();
  } else { } fi
  return 42;
}
""", "branch-local class owner requires destructor/drop lowering"),
    "experimental-owned-region": ("""
pub fnc main(): int {
  if (1 eq 1) {
    val x: owned int = 42;
  } else { } fi
  return 42;
}
""", "expected 'int'"),
}


def call(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=75)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-branch-local-") as folder:
        work = Path(folder)
        for name, source in GUESTS.items():
            path = work / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            compiled = call([args.compiler, str(path)])
            assert compiled.returncode == 0 and compiled.stdout, (name, compiled.stderr)
            assert "arm.stack" in compiled.stdout, (name, "missing lexical alloca")
            assert "llvm.lifetime.end" in compiled.stdout, (name, "missing scoped lifetime")
            ir = work / (name + ".ll")
            ir.write_text(compiled.stdout, encoding="utf-8")
            binary = work / name
            cmd = [args.clang, "-O1", "-x", "ir", str(ir), "-o", str(binary)]
            if args.sanitize:
                cmd[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            linked = call(cmd)
            assert linked.returncode == 0, (name, linked.stderr)
            ran = call([str(binary)])
            assert ran.returncode == 42, (name, ran.returncode, ran.stderr)
        for name, (source, diagnostic) in BAD.items():
            path = work / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            invalid = call([args.compiler, str(path)])
            assert invalid.returncode != 0 and not invalid.stdout, (
                name, "rejection emitted LLVM IR", invalid.stdout)
            assert diagnostic in invalid.stderr, (name, diagnostic, invalid.stderr)
    print("BRANCH_LOCAL_SCOPE_PASS", len(GUESTS), "native guests", len(BAD),
          "no-IR negatives", "ASAN_UBSAN" if args.sanitize else "")


if __name__ == "__main__":
    main()
