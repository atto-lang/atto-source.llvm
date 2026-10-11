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
  update(rt borrow mut p, 0);
  update(rt borrow mut p, 1);
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
  if (n eq 0) { update_first(rt borrow mut p); }
  else { update_second(rt borrow mut p); } fi
  return;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  dispatch(rt borrow mut p, 0);
  dispatch(rt borrow mut p, 1);
  return p.first + p.second;
}
""",
    "defer-after-join": PAIR + """
fnc add_one(borrow mut Pair p): void {
  p.first = p.first + 1;
  return;
}
fnc branch(borrow mut Pair p, int n): void {
  defer add_one(rt borrow mut p);
  if (n eq 0) { p.first = 10; } else { p.first = 40; } fi
  return;
}
pub fnc main(): int {
  let mut Pair p = new Pair();
  branch(rt borrow mut p, 0);
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
    "side-effect-condition": ("""
fnc one(): int { return 1; }
pub fnc main(): int {
  if (one() eq 1) { one(); } else { one(); } fi
  return 42;
}
""", "native guarded condition must be pure"),
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
""", "expected int expression"),
    "branch-local-defer": ("""
fnc update(): void { return; }
pub fnc main(): int {
  if (1 eq 1) { defer update(); } else { update(); } fi
  return 42;
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
            src = root / (name + ".ores")
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
