#!/usr/bin/env python3
"""Execute native bool semantics and require fail-closed type/ownership errors."""
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
    "atto-neq-and-bang-eq-alias": ("""
pub fnc main(): int {
  return (3 eq 3 && 3 neq 4 && 3 !eq 5) ? 42 : 7;
}
""", 42),
    "atto-neq-alias-comment-isolation": ("""
pub fnc main(): int {
  return (3 eq 3 && 3 !eq /* alias in block comment: !eq */ 4) ? 42 : 7;
}
""", 42),
    "bool-return-call": ("""
fnc yes(): bool {
  return true;
}
pub fnc main(): int {
  return yes() ? 42 : 7;
}
""", 42),
    "bool-param-local": ("""
fnc choose(bool flag, int x): int {
  const enabled: bool = flag && x > 10;
  return enabled ? x : 7;
}
pub fnc main(): int {
  return choose(true, 42);
}
""", 42),
    "comparison-family": ("""
pub fnc main(): int {
  return (3 < 4 && 4 <= 4 && 5 > 4 && 5 >= 5 && 5 != 4 && 5 == 5)
    ? 42 : 7;
}
""", 42),
    "generic-less-than-disambiguation": ("""
fnc less(int a, int b): bool {
  return a < b;
}
pub fnc main(): int {
  return less(1, 2) ? 42 : 7;
}
""", 42),
    "logical-precedence": ("""
pub fnc main(): int {
  return true || false && false ? 42 : 7;
}
""", 42),
    "logical-not": ("""
fnc invert(bool value): bool {
  return !value;
}
pub fnc main(): int {
  return invert(false) ? 42 : 7;
}
""", 42),
    "nested-bool-ternary": ("""
fnc invert(bool value): bool {
  return value ? false : true;
}
pub fnc main(): int {
  return invert(false) ? 42 : 7;
}
""", 42),
    "bool-guard-condition-call": ("""
fnc ready(): bool {
  return true;
}
fnc classify(): int {
  if (ready()) {
    return 42;
  } fi
  return 7;
}
pub fnc main(): int {
  return classify();
}
""", 42),
    "bool-statement-condition-call": (PAIR + """
fnc ready(): bool {
  return true;
}
fnc hit(borrow mut Pair p): void {
  p.first = 42;
  return;
}
fnc miss(borrow mut Pair p): void {
  p.first = 7;
  return;
}
fnc choose(borrow mut Pair p): int {
  if (ready()) {
    hit(rt mut borrow p);
  } else {
    miss(rt mut borrow p);
  } fi
  return p.first;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  return choose(rt mut borrow p);
}
""", 42),
    "bool-if-guard": ("""
fnc classify(bool flag, int x): int {
  if (flag && x > 10) {
    return 42;
  } fi
  return 7;
}
pub fnc main(): int {
  return classify(true, 12);
}
""", 42),
    "short-circuit-and-skips-rhs": (PAIR + """
fnc touch(borrow mut Pair p): bool {
  p.first = 99;
  return true;
}
fnc probe(borrow mut Pair p): bool {
  return false && touch(rt mut borrow p);
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  probe(rt mut borrow p);
  return p.first;
}
""", 1),
    "short-circuit-or-skips-rhs": (PAIR + """
fnc touch(borrow mut Pair p): bool {
  p.first = 99;
  return false;
}
fnc probe(borrow mut Pair p): bool {
  return true || touch(rt mut borrow p);
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  probe(rt mut borrow p);
  return p.first;
}
""", 1),
    "xor-evaluates-rhs": (PAIR + """
fnc touch(borrow mut Pair p): bool {
  p.first = 42;
  return true;
}
fnc probe(borrow mut Pair p): bool {
  return Xor(false, touch(rt mut borrow p));
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  probe(rt mut borrow p);
  return p.first;
}
""", 42),
}

NEGATIVE = {
    "atto-reject-spaced-bang-eq-alias": ("""
pub fnc main(): int {
  return (3 ! eq 4) ? 42 : 7;
}
""", None),
    "atto-reject-neq-identifier-prefix": ("""
pub fnc main(): int {
  return (3 !equivalent 4) ? 42 : 7;
}
""", None),
    "atto-reject-cross-type-word-equality": ("""
pub fnc main(): int {
  return (true eq 1) ? 42 : 7;
}
""", "equality operands require the same scalar type"),
    "reject-int-ternary-condition": ("""
pub fnc main(): int {
  return 1 ? 42 : 7;
}
""", "ternary condition requires bool"),
    "reject-bool-arithmetic": ("""
pub fnc main(): int {
  return true + 1;
}
""", "arithmetic requires int"),
    "reject-int-logical-not": ("""
pub fnc main(): bool {
  return !1;
}
""", "logical '!' requires bool"),
    "reject-int-logical": ("""
pub fnc main(): bool {
  return 1 && true;
}
""", "logical operator requires bool"),
    "reject-bool-return-mismatch": ("""
fnc bad(): bool {
  return 1;
}
pub fnc main(): int {
  return 0;
}
""", "native function return requires bool, got int"),
    "reject-bool-argument-mismatch": ("""
fnc choose(bool flag): int {
  return flag ? 42 : 7;
}
pub fnc main(): int {
  return choose(1);
}
""", "argument 'flag' requires bool, got int"),
    "reject-ternary-arm-mismatch": ("""
pub fnc main(): int {
  return true ? 42 : false;
}
""", "ternary arms require a common scalar type"),
    "reject-ordered-bool-comparison": ("""
pub fnc main(): bool {
  return true < false;
}
""", "ordered comparison requires int"),
    "reject-cross-type-equality": ("""
pub fnc main(): bool {
  return true == 1;
}
""", "equality operands require the same scalar type"),
    "reject-overlapping-bool-loans": ("""
fnc borrowed(borrow mut bool first, borrow mut bool second): int { return 0; }
pub fnc main(): int {
  val flag: bool = false;
  return borrowed(rt mut borrow flag, rt mut borrow flag);
}
""", "conflicting active native borrow"),
    "reject-hidden-call-in-declared-effects-arm": ("""
fnc leaf(): int {
  return 1;
}
fnc guarded(): int using io {
  if (true) {
    leaf();
  } else {
    leaf();
  } fi
  return 1;
}
pub fnc main(): int {
  return 0;
}
""", "using effects require transitive call summaries"),
    "reject-short-circuit-divergent-move": ("""
pub fnc main(): int {
  val x: int = 1;
  const flag: bool = false && (rt take x == 1);
  return x;
}
""", "ownership violation: short-circuit CFG predecessor ownership mismatch"),
}


def execute(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=75)


def compile_and_run(root, name, source, expected, compiler, clang, sanitize):
    src = root / (name + (".atto" if name.startswith("atto-") else ".ores"))
    src.write_text(source, encoding="utf-8")
    produced = execute([compiler, "--tree-shake", str(src)])
    if "define class" in source:
        # Main's conservative tree shaker deliberately rejects classes until
        # constructor roots are modeled. Still execute the unshaken program.
        assert produced.returncode != 0 and not produced.stdout, name
        assert "rejects class declarations until class initializer roots are modeled" in produced.stderr, (name, produced.stderr)
        produced = execute([compiler, str(src)])
    assert produced.returncode == 0 and produced.stdout, (name, produced.stderr)

    ll = root / (name + ".ll")
    ll.write_text(produced.stdout, encoding="utf-8")
    executable = root / name
    command = [clang, "-O1", "-x", "ir", str(ll), "-o", str(executable)]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    built = execute(command)
    assert built.returncode == 0, (name, built.stderr)
    ran = execute([str(executable)])
    assert ran.returncode == expected, (name, expected, ran.returncode, ran.stderr)

    if "short-circuit-" in name:
        assert "logic.rhs" in produced.stdout, name
        assert "logic.merge" in produced.stdout, name
    if name == "bool-param-local":
        assert "flag.bool.abi" in produced.stdout, name


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-bool-") as temp:
        root = Path(temp)
        for name, (source, expected) in POSITIVE.items():
            compile_and_run(root, name, source, expected, a.compiler, a.clang, a.sanitize)

        for name, (source, diagnostic) in NEGATIVE.items():
            src = root / (name + (".atto" if name.startswith("atto-") else ".ores"))
            src.write_text(source, encoding="utf-8")
            rejected = execute([a.compiler, str(src)])
            assert rejected.returncode != 0 and not rejected.stdout, (
                name, rejected.returncode, rejected.stdout)
            assert rejected.stderr.strip(), (name, "empty rejection diagnostic")
            if diagnostic is not None:
                assert diagnostic in rejected.stderr, (name, diagnostic, rejected.stderr)

    print(
        "PASS:",
        len(POSITIVE),
        "native bool executions and",
        len(NEGATIVE),
        "no-IR rejects",
        "ASan/UBSan" if a.sanitize else "",
    )


if __name__ == "__main__":
    main()
