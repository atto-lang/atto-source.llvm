#!/usr/bin/env python3
"""Execute native Oreslang ternaries and require fail-closed negatives."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

CONTRACT_PATH = (
    Path(__file__).resolve().parents[1]
    / "contracts"
    / "v1"
    / "conditional-expression-v1.json"
)
CONTRACT = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
assert CONTRACT["schema"] == "oreslang.conditional-expression.v1"
assert CONTRACT["if"]["produces_value"] is False
assert CONTRACT["ternary"]["condition_type"] == "bool"
assert CONTRACT["ternary"]["truthiness_coercion"] is False
assert CONTRACT["ternary"]["associativity"] == "right"
assert CONTRACT["ternary"]["evaluation"]["branches"] == "selected_arm_only"

PORTABLE_POSITIVE = {
    vector["id"]: (vector["source"], vector["result"])
    for vector in CONTRACT["portable_scalar_vectors"]
    if "result" in vector
}
PORTABLE_REJECT = {
    vector["id"]: vector
    for vector in CONTRACT["portable_scalar_vectors"]
    if "reject" in vector
}

PAIR = (
    "define class Pair as\n"
    "  pub let int first = 1;\n"
    "  pub let int second = 2;\n"
    "end\n"
)

POSITIVE = dict(PORTABLE_POSITIVE)
POSITIVE.update({
    "parenthesized-condition": ("""
pub fnc main(): int {
  return ((20 + 1 == 21)) ? 42 : 7;
}
""", 42),
    "local-initializer": ("""
pub fnc main(): int {
  const answer: int = 7 == 7 ? 40 : 1;
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
    "effectful-bool-condition": ("""
fnc one(): int {
  return 1;
}
pub fnc main(): int {
  return one() == 1 ? 42 : 7;
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
    ? select_true(rt mut borrow p)
    : select_false(rt mut borrow p);
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  choose(rt mut borrow p);
  return p.first;
}
""", 10),
})


NEGATIVE = {
    "reject-int-truthiness": (
        PORTABLE_REJECT["reject_integer_truthiness"]["source"],
        "ternary condition requires bool, got int",
    ),
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
    if "define class" in source:
        # Preserve main's conservative constructor-root admission boundary,
        # while retaining native execution coverage of class-valued effects.
        assert produced.returncode != 0 and not produced.stdout, name
        assert "rejects class declarations until class initializer roots are modeled" in produced.stderr, (name, produced.stderr)
        produced = execute([compiler, str(src)])
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
