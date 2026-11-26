#!/usr/bin/env python3
"""Prove a real BooleanOps.And source function shadows both intrinsic spellings."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def invoke(*args, env=None):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True,
                          text=True, env=env)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--cxx", required=True)
    p.add_argument("--source-root", required=True)
    a = p.parse_args()

    root = Path(a.source_root)
    emitted = invoke(
        a.compiler, root / "tests/fixtures/native-bool-shadow-aot.ores").stdout
    if "define i1 @BooleanOps.And(i1 " not in emitted:
        raise AssertionError("missing user-defined BooleanOps.And i1 definition")
    for symbol in ("BooleanOps.probe_unqualified", "BooleanOps.probe_qualified"):
        marker = f"define i1 @{symbol}"
        start = emitted.find(marker)
        if start < 0:
            raise AssertionError(f"missing shadow probe {symbol}")
        end = emitted.find("\n}", start)
        if end < 0 or "call i1 @BooleanOps.And" not in emitted[start:end + 2]:
            raise AssertionError(
                f"{symbol} was not preserved as a source call to BooleanOps.And")

    with tempfile.TemporaryDirectory(prefix="oreslang-bool-shadow-") as d:
        ir = Path(d) / "shadow.ll"
        ir.write_text(emitted, encoding="utf-8")
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        for level in (0, 2):
            flags = [f"-O{level}", "-g", "-fno-omit-frame-pointer",
                     "-fsanitize=address,undefined"]
            obj = Path(d) / f"shadow-O{level}.o"
            exe = Path(d) / f"shadow-O{level}"
            invoke(a.clang, *flags, "-c", "-x", "ir", ir, "-o", obj)
            invoke(a.cxx, "-std=c++17", *flags,
                   root / "tests/native_bool_shadow_guest_test.cpp",
                   obj, "-o", exe)
            invoke(exe, env=env)
            print(f"NATIVE_BOOL_SHADOW_ASAN_UBSAN_PASS opt={level}")


if __name__ == "__main__":
    main()
