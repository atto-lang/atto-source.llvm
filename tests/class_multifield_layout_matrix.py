#!/usr/bin/env python3
"""Exercise 1..16 field layouts through the real LLVM-to-native compiler path."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def execute(command, timeout=35):
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-mf-") as root:
        directory = Path(root)
        for count in range(1, 17):
            fields = "\n".join(f"  pub let int field{i} = {i};" for i in range(count))
            # The last field must be addressable even at the end of the
            # sixteen-element record, including across the borrow boundary.
            source = (f"define class Record as\n{fields}\nend\n"
                      f"fnc change(borrow mut Record target): void {{\n"
                      f"  target.field{count-1} = 42;\n  return;\n}}\n"
                      f"pub fnc main(): int {{\n"
                      f"  let mut Record record = new Record();\n"
                      f"  change(rt borrow mut record);\n"
                      f"  return record.field{count-1};\n}}\n")
            file = directory / f"layout-{count}.ores"
            file.write_text(source)
            compiled = execute([args.compiler, str(file)])
            assert compiled.returncode == 0, (count, compiled.stderr)
            assert not compiled.stderr, compiled.stderr
            assert "oreslang_owned_i64_region_" not in compiled.stdout, (
                count, "stack-only class unexpectedly introduced native heap ABI")
            ir = directory / f"layout-{count}.ll"
            ir.write_text(compiled.stdout)
            binary = directory / f"layout-{count}"
            built = execute([args.clang, "-O1", "-x", "ir", str(ir),
                             "-o", str(binary)], timeout=60)
            assert built.returncode == 0, (count, built.stderr)
            result = execute([str(binary)], timeout=10)
            assert result.returncode == 42 and not result.stderr, (
                count, result.returncode, result.stderr)
            print(f"PASS: class field count={count}, last field native mutation=42")
    print("PASS: 16/16 bounded native class field layouts executed")


if __name__ == "__main__":
    main()
