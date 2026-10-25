#!/usr/bin/env python3
"""Verify native Array<int> region C ABI against LLVM IR under Clang.

The fixture is handwritten LLVM IR: passing here does NOT establish compiler
source lowering of Oreslang Array<int> expressions or linked-queue parity.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys


def verify(clang: Path, source_root: Path, sanitizers: bool) -> None:
    header = source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.h"
    source = source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    fixture = source_root / "tests/fixtures/native-int-array-abi.ll"
    for path in (clang, header, source, fixture):
        if not path.is_file():
            raise ValueError(f"missing native array ABI test input: {path}")
    with tempfile.TemporaryDirectory(prefix="ores-native-int-array-llvm-") as tmp:
        exe = Path(tmp) / "abi-probe"
        command = [str(clang), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                   # A portable IR fixture deliberately has no target triple.
                   # Clang assigns the selected host triple; preserve -Werror
                   # for C while silencing only this expected IR-driver notice.
                   "-Wno-override-module",
                   "-x", "ir", str(fixture), "-x", "c", str(source),
                   "-I", str(source_root / "src/main/cpp"),
                   "-o", str(exe)]
        if sanitizers:
            command.extend(["-fno-omit-frame-pointer", "-fsanitize=address,undefined"])
        compiled = subprocess.run(command, capture_output=True, timeout=90, check=False)
        if compiled.returncode != 0:
            raise AssertionError(f"LLVM IR/Clang link failed: {compiled.stderr.decode(errors='replace')}")
        ran = subprocess.run([str(exe)], capture_output=True, timeout=30, check=False)
        if ran.returncode != 0 or ran.stdout or ran.stderr:
            raise AssertionError(f"native ABI control failed rc={ran.returncode}: "
                                 f"stdout={ran.stdout!r}, stderr={ran.stderr!r}")
    print("PASS: verified native LLVM opaque-pointer C ABI roundtrip", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.clang.resolve(), args.source_root.resolve(), args.sanitizers)
    except (OSError, AssertionError, ValueError, subprocess.TimeoutExpired) as e:
        print("native array ABI gate failed:", e, file=sys.stderr)
        sys.exit(1)
