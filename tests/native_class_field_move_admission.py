#!/usr/bin/env python3
"""Executable compiler admission regression for moved i64 fields and regions.

The bounded native class layout contains only trivially destructible i64
fields. Explicit field moves invalidate their place until reassignment;
unsafe whole-object borrows, double takes and region-owned transfers still
fail with no LLVM IR.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    accepts = 0
    rejects = 0
    with tempfile.TemporaryDirectory(prefix="ores-field-move-") as temp:
        folder = Path(temp)

        def check(source, diagnostic=None, experimental=False):
            nonlocal accepts, rejects
            fixture = folder / f"sample-{accepts + rejects}.ores"
            fixture.write_text(source)
            cmd = [args.compiler]
            if experimental:
                cmd.append("--experimental-owned-int")
            p = subprocess.run([*cmd, str(fixture)], capture_output=True,
                               text=True, timeout=30)
            if diagnostic:
                assert p.returncode != 0, (source, p.stdout)
                assert diagnostic in p.stderr, (source, p.stderr)
                assert not p.stdout.strip(), "emitted LLVM IR after rejection"
                rejects += 1
            else:
                assert p.returncode == 0, (source, p.stderr)
                assert p.stdout.startswith("; ModuleID"), (source, p.stdout)
                assert not p.stderr, (source, p.stderr)
                accepts += 1

        for count in (1, 2, 8, 16):
            decl = ("define class Record as\n" +
                    "".join(f"  pub let int f{i} = {i + 1};\n"
                            for i in range(count)) + "end\n")
            head = "pub fnc main(): int {\n  let Record p = new Record();\n"
            for result in (
                "  return rt take p.f0 + 41;\n",
                "  if (p.f0 eq 1) { return rt take p.f0 + 41; } fi\n"
                "  return p.f0 + 41;\n",
                "  const v: int = rt take p.f0;\n  p.f0 = v;\n"
                "  return p.f0 + 41;\n",
                "  return rt copy p.f0 + 41;\n",
                "  return rt take (p.f0 + 41);\n",
            ):
                check(decl + head + result + "}\n")
            for result, diag in (
                ("  const v: int = rt take p.f0;\n"
                 "  return p.f0 + v;\n", "use-after-move class field"),
                ("  const a: int = rt take p.f0;\n"
                 "  return rt take p.f0 + a;\n", "use-after-move class field"),
                ("  return rt take p.f0 + p.f0;\n", "use-after-move class field"),
            ):
                check(decl + head + result + "}\n", diag)
            check(decl +
                  "fnc view(borrow Record p): int { return p.f0; }\n" +
                  head + "  const v: int = rt take p.f0;\n"
                  "  return view(rt borrow p) + v;\n}\n",
                  "cannot borrow a partially moved class")
            check(decl +
                  "fnc view(borrow mut Record p): int { return rt take p.f0; }\n" +
                  head + "  return view(rt borrow mut p);\n}\n",
                  "cannot take a class field through a borrowed parameter")
            check(decl + head +
                  "  const v: int = rt take p.f0;\n"
                  "  let Record other = rt take p;\n"
                  "  return other.f0 + v;\n}\n",
                  "class move requires live unborrowed owner")

        owned = "pub fnc main(): int {\n  const x: owned int = 42;\n"
        for body in (
            "  return rt take x;\n",
            "  if (x eq 42) { return rt take x; } fi\n  return x;\n",
            "  const value: int = rt take x;\n  return value;\n",
        ):
            check(owned + body + "}\n",
                  "owned int take requires transfer/drop lowering", experimental=True)
        check(owned + "  return x;\n}\n", experimental=True)
        check("pub fnc main(): int {\n  const x: int = 42;\n"
              "  return rt take x;\n}\n")

    print(f"PASS {accepts} admitted reads/takes/reinitializations; "
          f"{rejects} unsafe operations rejected without IR")


if __name__ == "__main__":
    main()
