#!/usr/bin/env python3
"""Fail closed on native field/region takes until move, drop and CFG lowering exists.

A field is a Copy i64 *value* today, but `rt take p.field` is a move from an
addressable object place. LLVM currently emits a load for that syntax, without
marking the field uninitialized. Likewise, an experimental owned-int region
would unconditionally drop a slot after a source-level move. Neither is safe
to admit as a true ownership transfer.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def compile_source(compiler, path, *, experimental=False):
    command = [compiler]
    if experimental:
        command.append("--experimental-owned-int")
    command.append(str(path))
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    rejects = 0
    admits = 0
    with tempfile.TemporaryDirectory(prefix="ores-move-admission-") as directory:
        base = Path(directory)

        def check(source, *, diagnostic=None, experimental=False):
            nonlocal rejects, admits
            path = base / f"case-{rejects + admits}.ores"
            path.write_text(source)
            result = compile_source(args.compiler, path, experimental=experimental)
            if diagnostic:
                assert result.returncode != 0, (path, "unexpected acceptance")
                assert diagnostic in result.stderr, (path, result.stderr)
                assert not result.stdout.strip(), (path, "IR emitted after rejection")
                rejects += 1
            else:
                assert result.returncode == 0, (path, result.stderr)
                assert result.stdout.startswith("; ModuleID"), (path, result.stdout)
                assert not result.stderr, (path, result.stderr)
                admits += 1
            return result

        for field_count in (1, 2, 8, 16):
            declaration = (
                "define class Record as\n" +
                "".join(f"  pub let int f{i} = {i + 1};\n"
                        for i in range(field_count)) +
                "end\n"
            )
            prefix = "pub fnc main(): int {\n  let Record p = new Record();\n"
            endings = (
                "  return rt take p.f0;\n",
                "  if (p.f0 eq 1) { return rt take p.f0; } fi\n"
                "  return p.f0;\n",
                "  const x: int = rt take p.f0;\n  return x;\n",
                "  return 40 + rt take p.f0 + 1;\n",
            )
            for ending in endings:
                check(declaration + prefix + ending + "}\n",
                      diagnostic="ownership violation: class field take requires per-field move/drop lowering")
            check(declaration +
                  "fnc read(borrow Record p): int { return rt take p.f0; }\n"
                  "pub fnc main(): int { let Record p = new Record(); "
                  "return read(rt borrow p); }\n",
                  diagnostic="ownership violation: class field take requires per-field move/drop lowering")
            # Reading a field with explicit copy/share, or taking an i64
            # temporary computed from a field, is not a partial object move.
            check(declaration + prefix + "  return rt copy p.f0 + 41;\n}\n")
            check(declaration + prefix + "  return rt share p.f0 + 41;\n}\n")
            check(declaration + prefix + "  return rt take (p.f0 + 41);\n}\n")

        owned_prefix = "pub fnc main(): int {\n  const x: owned int = 42;\n"
        for ending in (
            "  return rt take x;\n",
            "  if (x eq 42) { return rt take x; } fi\n  return x;\n",
            "  const y: int = rt take x;\n  return y;\n",
        ):
            check(owned_prefix + ending + "}\n",
                  diagnostic="ownership violation: owned int take requires transfer/drop lowering",
                  experimental=True)
        check("fnc consume(int value): int { return value; }\n" +
              owned_prefix + "  return consume(rt take x);\n}\n",
              diagnostic="ownership violation: owned int take requires transfer/drop lowering",
              experimental=True)
        # Ordinary Copy i64 explicit moves and normal experimental region
        # use must remain admitted.
        check("pub fnc main(): int {\n"
              "  const x: int = 42;\n  return rt take x;\n}\n")
        check(owned_prefix + "  return x;\n}\n", experimental=True)

    print(f"PASS: {rejects} unsafe moves rejected without IR, "
          f"{admits} safe scalar/class reads admitted")


if __name__ == "__main__":
    main()
