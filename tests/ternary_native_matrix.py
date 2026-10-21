#!/usr/bin/env python3
"""Execute native Oreslang ternaries and require fail-closed negatives."""
import argparse
from pathlib import Path
import subprocess
import tempfile

PAIR = (
    "define class Pair as\n"
    "  pub let int first = 1;\n"
    "  pub let int second = 2;\n"
    "end\n"
)

POSITIVE = {
    "literal-true": ("""
pub fnc main(): int {
  return true ? 42 : 7;
}
""", 42),
    "literal-false": ("""
pub fnc main(): int {
  return false ? 42 : 7;
}
""", 7),
    "equality-and-arithmetic-precedence": ("""
pub fnc main(): int {
  return 20 + 1 == 21 ? 2 + 4 * 10 : 7;
}
""", 42),
    "right-associative": ("""
pub fnc main(): int {
  return 0 == 1 ? 1 : 2 == 2 ? 42 : 3;
}
""", 42),
    "parenthesized-condition": ("""
pub fnc main(): int {
  return ((20 + 1 == 21)) ? 42 : 7;
}
""", 42),
    "local-initializer": ("""
pub fnc main(): int {
  const answer = 7 == 7 ? 40 : 1;
  return answer + 2;
}
""", 42),
    "call-argument": ("""
fnc identity(int value): int {
  return value;
}
pub fnc main(): int {
  return identity(3 == 3 ? 42 : 7);
}
""", 42),
    "lazy-selected-arm-only": (PAIR + """
fnc select_true(borrow mut Pair p): int {
  p.first = 10;
  return 1;
}
fnc select_false(borrow mut Pair p): int {
  p.first = 99;
  return 2;
}
fnc choose(borrow mut Pair p): int {
  return true
    ? select_true(rt borrow mut p)
    : select_false(rt borrow mut p);
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  choose(rt borrow mut p);
  return p.first;
}
""", 10),
}

NEGATIVE = {
    "reject-int-truthiness": ("""
pub fnc main(): int {
  return 1 ? 42 : 7;
}
""", "native ternary condition requires bool"),
    "reject-equality-as-int-value": ("""
pub fnc main(): int {
  return 1 == 1;
}
""", "native scalar equality is currently only admitted as a ternary condition"),
    "reject-missing-colon": ("""
pub fnc main(): int {
  return true ? 42;
}
""", "expected ':'"),
    "reject-optional-param-still-gated": ("""
fnc choose(int? value): int {
  return value;
}
pub fnc main(): int {
  return choose(42);
}
""", "native optional type '?' requires versioned Option/default ABI lowering"),
    "reject-optional-presence-still-gated": ("""
fnc choose(int value?): int {
  return value;
}
pub fnc main(): int {
  return choose(42);
}
""", "native optional parameter presence '?' requires versioned default/presence ABI lowering"),
    "reject-effectful-condition": ("""
fnc one(): int {
  return 1;
}
pub fnc main(): int {
  return one() == 1 ? 42 : 7;
}
""", "native guarded condition must be pure and cannot move, call or borrow"),
    "reject-divergent-move-state": (PAIR + """
pub fnc main(): int {
  let mut Pair p = new Pair();
  return true ? rt take p.first : p.first;
}
""", "ownership violation: ternary CFG predecessor ownership mismatch"),
    "reject-void-branch": ("""
fnc noop(): void {
  return;
}
pub fnc main(): int {
  return true ? noop() : 7;
}
""", "void call cannot be used as an int expression"),
}


def execute(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=75)


def compile_and_run(root, name, source, expected, compiler, clang, sanitize):
    src = root / (name + ".ores")
    src.write_text(source)
    # Tree shaking is part of the semantic gate: callees referenced only
    # from ternary arms must remain reachable even though runtime executes one arm.
    produced = execute([compiler, "--tree-shake", str(src)])
    assert produced.returncode == 0 and produced.stdout, (name, produced.stderr)
    assert "ternary.true" in produced.stdout, name
    assert "ternary.false" in produced.stdout, name
    assert "ternary.merge" in produced.stdout, name
    assert "phi i64" in produced.stdout, name

    ll = root / (name + ".ll")
    ll.write_text(produced.stdout)
    executable = root / name
    command = [clang, "-O1", "-x", "ir", str(ll), "-o", str(executable)]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    built = execute(command)
    assert built.returncode == 0, (name, built.stderr)
    ran = execute([str(executable)])
    assert ran.returncode == expected, (name, expected, ran.returncode, ran.stderr)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-ternary-") as temp:
        root = Path(temp)
        for name, (source, expected) in POSITIVE.items():
            compile_and_run(root, name, source, expected, a.compiler, a.clang, a.sanitize)

        for name, (source, diagnostic) in NEGATIVE.items():
            src = root / (name + ".ores")
            src.write_text(source)
            rejected = execute([a.compiler, str(src)])
            assert rejected.returncode != 0 and not rejected.stdout, (
                name, rejected.returncode, rejected.stdout)
            assert diagnostic in rejected.stderr, (name, diagnostic, rejected.stderr)

    print(
        "PASS:",
        len(POSITIVE),
        "native ternary executions and",
        len(NEGATIVE),
        "no-IR rejects",
        "ASan/UBSan" if a.sanitize else "",
    )


if __name__ == "__main__":
    main()
