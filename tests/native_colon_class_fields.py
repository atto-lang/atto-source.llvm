#!/usr/bin/env python3
"""Compiler-gated colon/final class field subset; no IR on negative admission."""
import argparse
from pathlib import Path
import subprocess
import tempfile

SUCCESSES = {
    "canonical": """define class Foo as
  pub int a: 13
  pub final int b: 29
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return f.a + f.b;
}
""",
    "final-const-literal": """define class Foo as
  pub int a: 13
  pub final const int b: 29
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return f.a + f.b;
}
""",
    "compatibility": """define class Foo as
  pub int a = 13
  pub final int b = 29
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return f.a + f.b;
}
""",
    "legacy-let": """define class Foo as
  pub let int a: 13;
  pub let int b = 29;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  f.a = 13;
  return f.a + f.b;
}
""",
}
REJECTIONS = {
    "final-const-constructor-param-reject": ("""define class Foo as
  pub final const int a: input;
  constructor(int input) { return; }
end
pub fnc main(): int { let Foo f = new Foo(42); return f.a; }
""", "class field requires i64 literal initializer"),
    "private-default-field": ("""define class Foo as
  int a: 5
end
pub fnc main(): int {
  let Foo f = new Foo();
  return f.a;
}
""", "private class field"),
    "same-line-field-without-semicolon": ("""define class Foo as
  pub int a: 5 int b: 9
end
pub fnc main(): int { return 0; }
""", "class field requires ';' or a line break"),
    "final-const-runtime-reject": ("""define class Foo as
  pub final const int a: calculate();
end
pub fnc main(): int { return 0; }
""", "class field requires i64 literal initializer"),
    "final-const-expression-native-reject": ("""define class Foo as
  pub final const int a: (2 + 3);
end
pub fnc main(): int { return 0; }
""", "class field requires i64 literal initializer"),
    "final-const-overflow-reject": ("""define class Foo as
  pub final const int a: 9223372036854775808;
end
pub fnc main(): int { return 0; }
""", "integer literal overflows signed 64-bit result"),
    "final-const-reassign": ("""define class Foo as
  pub final const int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  f.a = 6;
  return f.a;
}
""", "cannot reassign final class field"),
    "final-const-take": ("""define class Foo as
  pub final const int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return rt take f.a;
}
""", "cannot take final class field"),
    "final-reassign": ("""define class Foo as
  pub final int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  f.a = 9;
  return f.a;
}
""", "cannot reassign final class field"),
    "final-take": ("""define class Foo as
  pub final int a: 42;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return rt take f.a;
}
""", "cannot take final class field"),
    "final-after-owner-move": ("""define class Foo as
  pub final int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  let mut Foo moved = rt take(f);
  moved.a = 9;
  return moved.a;
}
""", "cannot reassign final class field"),
    "final-conditional-write": ("""define class Foo as
  pub final int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  if (f.a == 5) {
    f.a = 6;
  } else {
    f.a = 7;
  } fi
  return f.a;
}
""", "cannot reassign final class field"),
    "bad-native-field-type": ("""define class Foo as
  string a: "100";
end
pub fnc main(): int { return 0; }
""", "expected 'int'"),
}

def main():
    arg = argparse.ArgumentParser()
    arg.add_argument("--compiler", required=True)
    arg.add_argument("--clang")
    opts = arg.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-colon-fields-") as folder:
        root = Path(folder)
        for name, source in SUCCESSES.items():
            src = root / f"{name}.ores"
            src.write_text(source, encoding="utf-8")
            result = subprocess.run([opts.compiler, str(src)], capture_output=True, text=True, timeout=60)
            if result.returncode != 0 or not result.stdout.startswith("; ModuleID"):
                raise AssertionError((name, result.returncode, result.stderr))
            if result.stderr:
                raise AssertionError((name, result.stderr))
            if opts.clang:
                ir = root / f"{name}.ll"
                ir.write_text(result.stdout, encoding="utf-8")
                exe = root / name
                compiled = subprocess.run([opts.clang, "-O1", "-x", "ir", str(ir), "-o", str(exe)],
                                          capture_output=True, text=True, timeout=60)
                if compiled.returncode != 0:
                    raise AssertionError((name, compiled.stderr))
                ran = subprocess.run([str(exe)], capture_output=True, timeout=60)
                if ran.returncode != 42:
                    raise AssertionError((name, ran.returncode, ran.stderr))
        for name, (source, diagnostic) in REJECTIONS.items():
            src = root / f"{name}.ores"
            src.write_text(source, encoding="utf-8")
            result = subprocess.run([opts.compiler, str(src)], capture_output=True, text=True, timeout=60)
            if result.returncode != 1 or result.stdout:
                raise AssertionError((name, result.returncode, result.stdout))
            if diagnostic not in result.stderr:
                raise AssertionError((name, diagnostic, result.stderr))
    print(f"PASS: {len(SUCCESSES)} class field forms, {len(REJECTIONS)} no-IR rejections")

if __name__ == "__main__":
    main()
