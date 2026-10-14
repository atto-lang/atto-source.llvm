#!/usr/bin/env python3
"""Compile Oreslang Boolean literal functions to real native LLVM machine code.

This intentionally does NOT exercise imported Assert, Any, Option or trap.
Each export is compiled from source using oreslang-llvmc, and called by a
separate native fixture, avoiding a handwritten implementation of its result.
"""
import argparse
import os
import re
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
    source = root / "tests/fixtures/primitive-bool-aot.ores"
    harness = root / "tests/native_bool_aot_guest_test.cpp"
    emitted = invoke(a.compiler, source).stdout
    if "ret i1 true" not in emitted or "ret i1 false" not in emitted:
        raise AssertionError("native bool result must use LLVM i1, not i64")
    # This fixture must be two closed, no-call i1 exports. Searching only
    # for malloc names misses indirect allocation, runtime helpers, and GC
    # calls with other symbol names. A later typed Boolean feature must
    # explicitly revise this contract rather than silently broaden it.
    if ("define i1 @BoolProbe.yes()" not in emitted or
            "define i1 @BoolProbe.no()" not in emitted):
        raise AssertionError("missing native Boolean guest exports")
    if re.search(r"(?m)^\s*declare\b", emitted):
        raise AssertionError("pure Boolean guest declares an external dependency")
    if re.search(r"(?m)^\s*(?:[%@][^=\n]*=\s*)?"
                 r"(?:(?:tail|musttail|notail)\s+)?(?:call|invoke|callbr)\b", emitted):
        raise AssertionError("pure Boolean guest contains an unexpected call")

    with tempfile.TemporaryDirectory(prefix="oreslang-bool-aot-") as d:
        ir = Path(d) / "guest.ll"
        ir.write_text(emitted, encoding="utf-8")
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        for level in (0, 2):
            flags = [f"-O{level}", "-g", "-fno-omit-frame-pointer",
                     "-fsanitize=address,undefined"]
            obj = Path(d) / f"guest-O{level}.o"
            exe = Path(d) / f"bool-O{level}"
            invoke(a.clang, *flags, "-c", "-x", "ir", ir, "-o", obj)
            invoke(a.cxx, "-std=c++17", *flags, harness, obj, "-o", exe)
            invoke(exe, env=env)
            print(f"NATIVE_TYPED_BOOL_AOT_ASAN_UBSAN_PASS opt={level}")


if __name__ == "__main__":
    main()
