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

                # Positive aliasing: multiple shared whole-object loans
                # are allowed, even when each reads a different field.
                shared = (f"define class Record as\n{fields}\nend\n"
                          f"fnc inspect(borrow Record left, borrow Record right): int {{\n"
                          f"  return left.field0 + right.field{count-1} + {43-count};\n}}\n"
                          f"pub fnc main(): int {{\n"
                          f"  let mut Record record = new Record();\n"
                          f"  return inspect(rt borrow record, rt borrow record);\n}}\n")
                shared_file = directory / f"shared-shared-{count}.ores"
                shared_file.write_text(shared)
                shared_ir = execute([args.compiler, str(shared_file)])
                if (shared_ir.returncode or shared_ir.stderr or
                        "oreslang_owned_i64_region_" in shared_ir.stdout):
                    raise AssertionError((count, "two shared loans rejected or allocated",
                                          shared_ir.stderr))
                shared_ir_path = directory / f"shared-shared-{count}.ll"
                shared_ir_path.write_text(shared_ir.stdout)
                shared_exe = directory / f"shared-shared-{count}"
                build = execute([args.clang, "-O1", "-x", "ir",
                                 str(shared_ir_path), "-o", str(shared_exe)], timeout=60)
                if build.returncode:
                    raise AssertionError((count, "shared/shared native build", build.stderr))
                result = execute([str(shared_exe)], timeout=10)
                if result.returncode != 42 or result.stderr:
                    raise AssertionError((count, "shared/shared execution",
                                          result.returncode, result.stderr))

                # All acquisition orders and exclusive/exclusive must reject
                # at compile time with no partial IR, independent of which
                # class field each callee accesses.
                overlap_shapes = (
                    ("mutable_shared", "borrow mut", "borrow",
                     "rt borrow mut", "rt borrow"),
                    ("shared_mutable", "borrow", "borrow mut",
                     "rt borrow", "rt borrow mut"),
                    ("mutable_mutable", "borrow mut", "borrow mut",
                     "rt borrow mut", "rt borrow mut"),
                )
                for label, left_mode, right_mode, left_arg, right_arg in overlap_shapes:
                    invalid = (f"define class Record as\n{fields}\nend\n"
                               f"fnc conflict({left_mode} Record left, {right_mode} Record right): int {{\n"
                               f"  return left.field0 + right.field{count-1};\n}}\n"
                               f"pub fnc main(): int {{\n"
                               f"  let mut Record record = new Record();\n"
                               f"  return conflict({left_arg} record, {right_arg} record);\n}}\n")
                    negative_file = directory / f"reject-{label}-{count}.ores"
                    negative_file.write_text(invalid)
                    rejected = execute([args.compiler, str(negative_file)])
                    if (rejected.returncode == 0 or rejected.stdout.strip() or
                            "conflicting active native borrow" not in rejected.stderr):
                        raise AssertionError((count, label, "alias must reject before any IR",
                                              rejected.returncode, rejected.stdout,
                                              rejected.stderr))
                # Independent owners must NOT be conflated by an overbroad
                # whole-object loan check. Include a real native write/read
                # through two differently borrowed class instances.
                if count in (2, 8, 16):
                    independent = (f"define class Record as\n{fields}\nend\n"
                                   f"fnc combine(borrow mut Record left, borrow Record right): int {{" 
                                   f"\n  left.field0 = 40;"
                                   f"\n  return left.field0 + right.field{count-1} + {3-count};\n}}\n"
                                   f"pub fnc main(): int {{\n"
                                   f"  let mut Record left = new Record();\n"
                                   f"  let mut Record right = new Record();\n"
                                   f"  return combine(rt borrow mut left, rt borrow right);\n}}\n")
                    positive = directory / f"distinct-owner-{count}.ores"
                    positive.write_text(independent)
                    emitted = execute([args.compiler, str(positive)])
                    if (emitted.returncode or emitted.stderr or
                            "oreslang_owned_i64_region_" in emitted.stdout):
                        raise AssertionError((count, "disjoint owners incorrectly conflict",
                                              emitted.returncode, emitted.stderr))
                    ll = directory / f"distinct-owner-{count}.ll"
                    ll.write_text(emitted.stdout)
                    exe = directory / f"distinct-owner-{count}"
                    linked = execute([args.clang, "-O1", "-x", "ir", str(ll),
                                      "-o", str(exe)], timeout=60)
                    if linked.returncode:
                        raise AssertionError((count, "distinct-owner link", linked.stderr))
                    ran = execute([str(exe)], timeout=10)
                    if ran.returncode != 42 or ran.stderr:
                        raise AssertionError((count, "distinct-owner native outcome",
                                              ran.returncode, ran.stderr))

                    # An exclusive loan ends at the call boundary. Two
                    # successive calls borrowing the same owner are legal.
                    successive = source.replace(
                        "  change(rt borrow mut record);\n",
                        "  change(rt borrow mut record);\n"
                        "  change(rt borrow mut record);\n")
                    if successive == source:
                        raise AssertionError((count, "missing call site in template"))
                    subsequent = directory / f"successive-exclusive-{count}.ores"
                    subsequent.write_text(successive)
                    emitted = execute([args.compiler, str(subsequent)])
                    if (emitted.returncode or emitted.stderr or
                            "oreslang_owned_i64_region_" in emitted.stdout):
                        raise AssertionError((count, "ended loan leaks into next call",
                                              emitted.returncode, emitted.stderr))
                    ll = directory / f"successive-exclusive-{count}.ll"
                    ll.write_text(emitted.stdout)
                    exe = directory / f"successive-exclusive-{count}"
                    linked = execute([args.clang, "-O1", "-x", "ir", str(ll),
                                      "-o", str(exe)], timeout=60)
                    if linked.returncode:
                        raise AssertionError((count, "successive-exclusive link", linked.stderr))
                    ran = execute([str(exe)], timeout=10)
                    if ran.returncode != 42 or ran.stderr:
                        raise AssertionError((count, "successive-exclusive native outcome",
                                              ran.returncode, ran.stderr))
                    print(f"PASS: distinct-owner and successive-exclusive native cases for {count} fields")
                print(f"PASS: {count}-field projections, shared/shared, 3 whole-owner overlap orders")
    print("PASS: 16 layouts, 29 interior projections, 15 shared/shared executions, 45 alias rejections, 6 owner-independence/lifetime executions")


if __name__ == "__main__":
    main()
