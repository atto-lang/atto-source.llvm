#!/usr/bin/env python3
"""Execute source-exact primitive class field borrow regression corpus.

All successful programs must compile to native LLVM IR and return 42 under
Clang. Every negative case must fail without emitting *any* LLVM IR.
This intentionally does not claim disjoint mutable field-loan support.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


POSITIVE = (
    "class-projection-shared-native.ores",
    "class-projection-exclusive-native.ores",
    "class-projection-shared-reborrow-native.ores",
    "class-projection-two-shared-native.ores",
)
NEGATIVE = {
    "class-projection-overlap-reject.ores": "conflicting active native borrow",
    "class-projection-immutable-mut-reject.ores": "cannot mutably borrow immutable owner",
    "class-projection-moved-field-reject.ores": "cannot borrow a moved class field",
    "class-projection-private-reject.ores": "private class field",
    "class-projection-type-mismatch-reject.ores": "borrowed argument type mismatch",
    "class-projection-whole-object-conflict-reject.ores": "conflicting active native borrow",
}


def run(argv, *, timeout=35):
    return subprocess.run(argv, text=True, capture_output=True,
                          check=False, timeout=timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    options = parser.parse_args()
    completed = 0
    for filename in POSITIVE:
        source = options.fixtures / filename
        compiled = run([options.compiler, str(source)])
        if compiled.returncode != 0 or not compiled.stdout.strip() or compiled.stderr:
            raise RuntimeError(f"{filename}: valid guest rejected: {compiled.stderr}")
        if "oreslang_owned_i64_region_" in compiled.stdout or "gc.statepoint" in compiled.stdout:
            raise RuntimeError(f"{filename}: unexpected heap or tracing-GC ABI")
        with tempfile.TemporaryDirectory(prefix="ores-field-projection-") as path:
            module = Path(path) / "guest.ll"
            binary = Path(path) / "guest"
            module.write_text(compiled.stdout)
            linked = run([options.clang, "-O1", "-x", "ir", str(module), "-o", str(binary)])
            if linked.returncode != 0:
                raise RuntimeError(f"{filename}: native Clang failure: {linked.stderr}")
            executed = run([str(binary)], timeout=8)
            if executed.returncode != 42 or executed.stderr:
                raise RuntimeError(
                    f"{filename}: native exit={executed.returncode}, stderr={executed.stderr}")
        completed += 1
    for filename, diagnostic in NEGATIVE.items():
        source = options.fixtures / filename
        compiled = run([options.compiler, str(source)])
        if compiled.returncode == 0 or compiled.stdout.strip():
            raise RuntimeError(f"{filename}: UNSAFE negative source compiled or emitted IR")
        if diagnostic not in compiled.stderr:
            raise RuntimeError(
                f"{filename}: expected {diagnostic!r}; got {compiled.stderr!r}")
        completed += 1
    print(f"PASS {completed} native projection borrow cases "
          f"({len(POSITIVE)} Clang executions, {len(NEGATIVE)} no-IR rejections)")


if __name__ == "__main__":
    main()
