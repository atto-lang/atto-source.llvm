#!/usr/bin/env python3
"""Execute native Oreslang conditional CFGs and require fail-closed negatives."""
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
    "field-take-and-reinitialize-on-both-edges": PAIR + """
pub fnc main(): int {
  let mut Pair p = new Pair();
  if (p.first eq 1) { p.first = rt take p.second; p.second = 40; }
  else { p.first = rt take p.second; p.second = 40; } fi
  return p.first + p.second;
}
""",
    "both-arms": PAIR + """
fnc update(borrow mut Pair p, int flag): void {
  if (flag == 0) { p.first = 12; } else { p.second = 30; } fi
  return;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  update(rt mut borrow p, 0);
  update(rt mut borrow p, 1);
  return p.first + p.second;
}
""",
    "call-arms": PAIR + """
fnc update_first(borrow mut Pair p): void {
  p.first = 12;
  return;
}
fnc update_second(borrow mut Pair p): void {
  p.second = 30;
  return;
}
fnc dispatch(borrow mut Pair p, int n): void {
  if (n eq 0) { update_first(rt mut borrow p); }
  else { update_second(rt mut borrow p); } fi
  return;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  dispatch(rt mut borrow p, 0);
  dispatch(rt mut borrow p, 1);
  return p.first + p.second;
}
""",
    "defer-after-join": PAIR + """
fnc add_one(borrow mut Pair p): void {
  p.first = p.first + 1;
  return;
}
fnc branch(borrow mut Pair p, int n): void {
  defer add_one(rt mut borrow p);
  if (n eq 0) { p.first = 10; } else { p.first = 40; } fi
  return;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  branch(rt mut borrow p, 0);
  return p.first + 31;
}
""",
}

NEGATIVE = {
    "divergent-field-move": (PAIR + """
pub fnc main(): int {
  let mut Pair p = new Pair();
  if (p.first eq 1) { p.first = rt take p.second; }
  else { p.first = 41; } fi
  return p.first;
}
""", "ownership violation: conditional CFG predecessor ownership mismatch"),
    "divergent-ownership": ("""
fnc consume(int n): void { return; }
pub fnc main(): int {
  val n: int = 42;
  if (n eq 42) { consume(rt take n); }
  else { consume(n); } fi
  return n;
}
""", "ownership violation: conditional CFG predecessor ownership mismatch"),
    "missing-else": (PAIR + """
pub fnc main(): int {
  let mut Pair p = new Pair();
  if (1 eq 1) { p.first = 42; } fi
  return p.first;
}
""", "expected 'else'"),
    "branch-local-construction": (PAIR + """
pub fnc main(): int {
  if (1 eq 1) { let mut Pair p = new Pair(); } else { } fi
  return 42;
}
""", "native branch-local class owner requires destructor/drop lowering"),
    "branch-local-defer": ("""
fnc update(): void { return; }
pub fnc main(): int {
  if (1 eq 1) { defer update(); } else { update(); } fi
  return 42;
}
""", "expected int expression"),
    "if-as-binding-value": ("""
pub fnc main(): int {
  val y: int = if (1 eq 1) { return 1; } else { return 2; } fi
  return y;
}
""", "expected int expression"),
    "if-as-return-value": ("""
pub fnc main(): int {
  return if (1 eq 1) { return 1; } else { return 2; } fi;
}
""", "expected int expression"),
    "branch-after-return-guard": (PAIR + """
pub fnc main(): int {
  let mut Pair p = new Pair();
  if (0 eq 1) { return 0; } fi
  if (1 eq 1) { p.first = 4; } else { p.first = 5; } fi
  return 42;
}
""", "native conditional statements cannot follow return guards"),
}


# Conditions execute once before splitting the affine ownership state.
POSITIVE["side-effect-condition"] = '\nfnc one(): int { return 1; }\npub fnc main(): int {\n  if (one() eq 1) { one(); } else { one(); } fi\n  return 42;\n}\n'



# Canonical .atto guard is an inverted, guaranteed-terminal conditional.
# These must be compiler-backed guest executions, not host parser simulations.
POSITIVE["guard-success-fallthrough"] = """
fnc bounded(int x): int {
  guard x > 0 { return 1 }
  guard x < 10, x != 5 { return 2; }
  return 42;
}
pub fnc main(): int {
  return bounded(3);
}
"""

POSITIVE["guard-nested-in-conditional"] = """
fnc bounded(int x): int {
  if (x > 0) {
    guard x < 10 { return 2; }
  } else { return 1; } fi
  return 42;
}
pub fnc main(): int { return bounded(3); }
"""


POSITIVE["guard-multistatement-terminal"] = """
fnc one(): int { return 1; }
fnc bounded(int x): int {
  guard x > 0 {
    one();
    return 42;
  }
  return 42;
}
pub fnc main(): int {
  return bounded(-1);
}
"""

POSITIVE["guard-nested-exhaustive-terminal"] = """
fnc bounded(int x): int {
  guard x > 0 {
    if (x == 0) { return 42; } else { return 42; } fi
  }
  return 42;
}
pub fnc main(): int {
  return bounded(-1);
}
"""


POSITIVE["guard-enclosing-then-else"] = """
fnc bounded(int x): int {
  if (x > 0); then
    guard x < 10 { return 1; }
  else
    return 2;
  fi
  return 42;
}
pub fnc main(): int { return bounded(3); }
"""

NEGATIVE["guard-incomplete-nested-if"] = ("""
pub fnc main(): int {
  guard false {
    if (true) { return 42; } fi
  }
  return 42;
}
""", "AttoLang guard failure block must terminate on every path")

NEGATIVE["guard-terminal-call-only"] = ("""
fnc noop(): void { return; }
pub fnc main(): int {
  guard false {
    noop();
  }
  return 42;
}
""", "AttoLang guard failure block must terminate on every path")

NEGATIVE["guard-missing-exit"] = ("""
pub fnc main(): int {
  guard 1 > 0 { }
  return 42;
}
""", "AttoLang guard failure block must terminate on every path")

NEGATIVE["guard-no-else"] = ("""
pub fnc main(): int {
  guard 1 > 0 { return 1; } else { return 2; }
  return 42;
}
""", "AttoLang guard has no else or fi")

NEGATIVE["guard-no-fi"] = ("""
pub fnc main(): int {
  guard 1 > 0 { return 1; } fi
  return 42;
}
""", "AttoLang guard has no else or fi")

NEGATIVE["guard-no-return-followthrough"] = ("""
pub fnc main(): int {
  guard 1 > 0 { return 1; return 2; }
  return 42;
}
""", "native statement follows terminal return")


NEGATIVE["guard-option-narrowing-denied"] = ("""
pub fnc main(): int {
  let o: Option<int> = Some(4);
  guard o is Some { return 1; }
  return 42;
}
""", "native guard is Some requires ownership-aware Option narrowing")

NEGATIVE["guard-class-return-denied"] = ("""
define class Box as
  pub let int value = 42;
end
fnc create(int n): Box {
  guard n > 0 { return new Box(); }
  return new Box();
}
pub fnc main(): int {
  return 42;
}
""", "structured class return requires ownership-aware exit lowering")


# Execute the same Git-blob-identical canonical .atto files registered in all
# five AttoLang compiler repositories; this runs the actual native frontend.
GUARD_CORPUS = Path(__file__).resolve().parents[1] / "contracts" / "guard-v1"
POSITIVE["guard-canonical-scalar"] = (GUARD_CORPUS / "canonical-scalar.atto").read_text(encoding="utf-8")
NEGATIVE["guard-canonical-fallthrough"] = (
    (GUARD_CORPUS / "reject-fallthrough.atto").read_text(encoding="utf-8"),
    "AttoLang guard failure block must terminate on every path",
)
NEGATIVE["guard-canonical-else"] = (
    (GUARD_CORPUS / "reject-else.atto").read_text(encoding="utf-8"),
    "AttoLang guard has no else or fi",
)

def execute(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=75)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-conditional-cfg-") as temp:
        root = Path(temp)
        for name, source in POSITIVE.items():
            src = root / (name + (".atto" if name.startswith("guard-") else ".ores"))
            src.write_text(source)
            produced = execute([a.compiler, str(src)])
            assert produced.returncode == 0 and produced.stdout, (name, produced.stderr)
            assert "if.then" in produced.stdout and "if.else" in produced.stdout, name
            ll = root / (name + ".ll")
            ll.write_text(produced.stdout)
            executable = root / name
            command = [a.clang, "-O1", "-x", "ir", str(ll), "-o", str(executable)]
            if a.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            result = execute(command)
            assert result.returncode == 0, (name, result.stderr)
            ran = execute([str(executable)])
            assert ran.returncode == 42, (name, ran.returncode, ran.stderr)
        for name, (source, diagnostic) in NEGATIVE.items():
            src = root / (name + ".ores")
            src.write_text(source)
            rejected = execute([a.compiler, str(src)])
            assert rejected.returncode != 0 and not rejected.stdout, (
                name, rejected.returncode, rejected.stdout)
            assert diagnostic in rejected.stderr, (name, diagnostic, rejected.stderr)
    print("PASS:", len(POSITIVE), "native if/else CFG executions and",
          len(NEGATIVE), "no-IR rejects", "ASan/UBSan" if a.sanitize else "")


if __name__ == "__main__":
    main()
