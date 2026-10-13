#!/usr/bin/env python3
"""Compiler-gated colon/final class field subset; no IR on negative admission."""
import argparse
from pathlib import Path
import subprocess
import tempfile

SUCCESSES = {
    "canonical": """define class Foo as
  int a: 13;
  final int b: 29;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return f.a + f.b;
}
""",
    "compatibility": """define class Foo as
  int a = 13;
  final int b = 29;
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
    "final-reassign": ("""define class Foo as
  final int a: 5;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  f.a = 9;
  return f.a;
}
""", "cannot reassign final class field"),
    "final-take": ("""define class Foo as
  final int a: 42;
end
pub fnc main(): int {
  let mut Foo f = new Foo();
  return rt take f.a;
}
""", "cannot take final class field"),
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
            assert result.returncode == 0 and result.stdout.startswith("; ModuleID"), (
                name, result.returncode, result.stderr)
            assert not result.stderr, (name, result.stderr)
            if opts.clang:
                ir = root / f"{name}.ll"
                ir.write_text(result.stdout, encoding="utf-8")
                exe = root / name
                compiled = subprocess.run([opts.clang, "-O1", "-x", "ir", str(ir), "-o", str(exe)],
                                          capture_output=True, text=True, timeout=60)
                assert compiled.returncode == 0, (name, compiled.stderr)
                ran = subprocess.run([str(exe)], capture_output=True, timeout=60)
                assert ran.returncode == 42, (name, ran.returncode, ran.stderr)
        for name, (source, diagnostic) in REJECTIONS.items():
            src = root / f"{name}.ores"
            src.write_text(source, encoding="utf-8")
            result = subprocess.run([opts.compiler, str(src)], capture_output=True, text=True, timeout=60)
            assert result.returncode != 0 and not result.stdout, (name, result.returncode, result.stdout)
            assert diagnostic in result.stderr, (name, diagnostic, result.stderr)
    print(f"PASS: {len(SUCCESSES)} class field forms, {len(REJECTIONS)} no-IR rejections")

if __name__ == "__main__":
    main()
