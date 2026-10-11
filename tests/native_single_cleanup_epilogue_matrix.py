#!/usr/bin/env python3
"""Verify a single LLVM cleanup epilogue over every admitted return path.

The compiler must not duplicate a 64-owner conditional drop ladder on
8 guarded returns. Also execute value snapshots and moved region slot
reuse through real native Clang with optional ASan/UBSan.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def command(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=120)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--runtime", required=True)
    p.add_argument("--include", required=True)
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()

    examples = []
    for count in (1, 8, 64):
        locals_ = "".join(
            f"  const slot{i}: owned int = {i + 1};\n" for i in range(count))
        guards = "".join(
            f"  if (slot0 eq {1000 + i}) {{ return 0; }} fi\n"
            for i in range(8))
        examples.append((f"owned-{count}-eight-guards",
            f"pub fnc main(): int {{\n{locals_}{guards}  return 42;\n}}\n",
            count, 42))

    # All exits take the same original ownership through a different CFG
    # branch. A single cleanup epilogue must see only a disarmed flag.
    examples.append(("move-on-any-exit",
        "pub fnc main(): int {\n"
        "  const x: owned int = 42;\n" +
        "".join(
            f"  if (x eq {k}) {{ return rt take x; }} fi\n"
            for k in range(35, 43)) +
        "  return rt take x;\n}\n", 1, 42))

    # A later allocation may reuse the exact same native region slot.
    # It must get an independent live flag, and the old owner cannot drop it.
    examples.append(("move-then-slot-reuse",
        "pub fnc main(): int {\n"
        "  const x: owned int = 42;\n"
        "  const y: owned int = rt take x;\n"
        "  if (y eq 0) { return 0; } fi\n"
        "  return rt take y;\n}\n", 2, 42))

    cls = ("define class Counter as\n  pub let int value = 40;\nend\n"
           "fnc bump(borrow mut Counter c): void {\n"
           "  c.value = c.value + 1;\n  return;\n}\n"
           "fnc snapshot(borrow mut Counter c, int mode): int {\n"
           "  defer bump(rt borrow mut c);\n"
           "  if (mode eq 0) { return c.value; } fi\n"
           "  return c.value;\n}\n"
           "pub fnc main(): int {\n"
           "  let mut Counter c = new Counter();\n"
           "  const a: int = snapshot(rt borrow mut c, 0);\n"
           "  const b: int = snapshot(rt borrow mut c, 1);\n"
           "  return a + b - 39;\n}\n")
    examples.append(("deferred-mutator-return-snapshot", cls, 0, 42))

    with tempfile.TemporaryDirectory(prefix="ores-single-cleanup-") as td:
        root = Path(td)
        for label, source, owner_count, expected in examples:
            guest = root / (label + ".ores")
            guest.write_text(source)
            cmd = [args.compiler, str(guest)]
            if owner_count:
                cmd.insert(1, "--experimental-owned-int")
            compiled = command(cmd)
            assert compiled.returncode == 0, (label, compiled.stderr)
            ir = compiled.stdout
            # LLVM basic-block labels have function-local scope. A module
            # containing helper functions legitimately has one epilogue in
            # each function; counting the whole module yields false failures.
            functions = re.findall(r"(?ms)^define\b[^\n]*\{\n.*?^\}", ir)
            assert functions, (label, "no defined LLVM functions")
            # Direct-return helper functions without guarded returns or
            # defers require no additional epilogue; each multi-exit
            # function still has exactly one shared cleanup block.
            counts = [fn.count("exit.cleanup:") for fn in functions]
            assert all(count <= 1 for count in counts), (
                label, "duplicate cleanup epilogue", counts)
            assert any(count == 1 for count in counts), (
                label, "missing guarded-return cleanup", counts)
            assert ir.count("return.snapshot.slot") >= 1, (label, "return not captured")
            if owner_count:
                close_calls = re.findall(
                    r"\bcall void @oreslang_owned_i64_region_close\(", ir)
                drop_calls = re.findall(
                    r"\bcall void @oreslang_owned_i64_region_drop_handle\(", ir)
                # Move-time drops can add call sites, but the common cleanup
                # ladder contributes exactly owner_count call sites.
                assert len(close_calls) == 1, (label, "duplicate region close", len(close_calls))
                assert len(drop_calls) >= owner_count, (label, "lost owners")
                assert len(drop_calls) <= owner_count + 9, (label, "duplicated cleanup ladder", len(drop_calls))
            ll = root / (label + ".ll")
            ll.write_text(ir)
            program = root / (label + ".bin")
            cc = [args.clang, "-O1", "-fno-omit-frame-pointer"]
            if args.sanitize:
                cc.extend(["-fsanitize=address,undefined", "-g"])
            cc += ["-x", "ir", str(ll)]
            if owner_count:
                cc += ["-x", "c", args.runtime, "-I", args.include, "-std=c11"]
            cc += ["-o", str(program)]
            built = command(cc)
            assert built.returncode == 0, (label, built.stderr)
            ran = command([str(program)])
            assert ran.returncode == expected, (label, ran.returncode, ran.stderr)

    print(f"PASS {len(examples)} native return-snapshot and shared-cleanup "
          f"source programs; sanitizers={args.sanitize}")


if __name__ == "__main__":
    main()
