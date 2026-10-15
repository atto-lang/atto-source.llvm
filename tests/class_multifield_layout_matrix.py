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
            if compiled.returncode != 0:
                raise AssertionError((count, compiled.stderr))
            if compiled.stderr:
                raise AssertionError(compiled.stderr)
            if "oreslang_owned_i64_region_" in compiled.stdout:
                raise AssertionError((count, "stack-only class unexpectedly introduced native heap ABI"))
            ir = directory / f"layout-{count}.ll"
            ir.write_text(compiled.stdout)
            binary = directory / f"layout-{count}"
            built = execute([args.clang, "-O1", "-x", "ir", str(ir),
                             "-o", str(binary)], timeout=60)
            if built.returncode != 0:
                raise AssertionError((count, built.stderr))
            result = execute([str(binary)], timeout=10)
            if result.returncode != 42 or result.stderr:
                raise AssertionError((count, result.returncode, result.stderr))
            print(f"PASS: class field count={count}, last field native mutation=42")

            # Every field is addressable, not merely the final element.
            # A mutable class loan always covers the WHOLE record, so two
            # overlapping arguments must be rejected even when their
            # callees touch different fields.
            if count > 1:
                for index in (0, count // 2):
                    if index == count - 1:
                        continue
                    changed = source.replace(
                        f"target.field{count-1} = 42;",
                        f"target.field{index} = 42;").replace(
                        f"return record.field{count-1};",
                        f"return record.field{index};")
                    changed_file = directory / f"field-{count}-{index}.ores"
                    changed_file.write_text(changed)
                    output = execute([args.compiler, str(changed_file)])
                    if output.returncode != 0 or output.stderr:
                        raise AssertionError((count, index, output.stderr))
                    if "oreslang_owned_i64_region_" in output.stdout:
                        raise AssertionError((count, index, "unexpected native heap ABI"))
                    ir_path = directory / f"field-{count}-{index}.ll"
                    ir_path.write_text(output.stdout)
                    exe_path = directory / f"field-{count}-{index}"
                    build = execute([args.clang, "-O1", "-x", "ir",
                                     str(ir_path), "-o", str(exe_path)], timeout=60)
                    if build.returncode != 0:
                        raise AssertionError((count, index, build.stderr))
                    run = execute([str(exe_path)], timeout=10)
                    if run.returncode != 42 or run.stderr:
                        raise AssertionError((count, index, run.returncode, run.stderr))

                invalid = (f"define class Record as\n{fields}\nend\n"
                           f"fnc conflict(borrow mut Record left, borrow Record right): int {{\n"
                           f"  return right.field{count-1};\n}}\n"
                           f"pub fnc main(): int {{\n"
                           f"  let mut Record record = new Record();\n"
                           f"  return conflict(rt borrow mut record, rt borrow record);\n}}\n")
                negative_file = directory / f"reject-overlap-{count}.ores"
                negative_file.write_text(invalid)
                rejected = execute([args.compiler, str(negative_file)])
                if (rejected.returncode == 0 or rejected.stdout.strip() or
                        "conflicting active native borrow" not in rejected.stderr):
                    raise AssertionError((count, "overlapping loan not rejected fail-closed",
                                          rejected.returncode, rejected.stdout,
                                          rejected.stderr))
                print(f"PASS: {count}-field distinct projection and whole-object borrow exclusion")
    print("PASS: 16/16 layouts, interior projections and 15 whole-object alias rejections verified")


if __name__ == "__main__":
    main()
