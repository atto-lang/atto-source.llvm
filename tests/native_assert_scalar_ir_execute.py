#!/usr/bin/env python3
"""CTest: execute real LLVM-generated Assert ABI IR against the native model.

This is primitive host ABI verification, not compiled Oreslang std/assert guest
import/linking or actor supervision parity. It must fail on any skipped phase.
"""
from __future__ import annotations

import argparse
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def execute(cmd: list[str], **kwargs: object) -> subprocess.CompletedProcess:
    completed = subprocess.run(cmd, check=False, timeout=120, **kwargs)
    require(completed.returncode == 0, f"exit {completed.returncode}: {cmd!r}; stderr={completed.stderr}")
    return completed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--opt", choices=("0", "2"), required=True)
    parser.add_argument("--pointer-mode", choices=("typed", "opaque"), required=True)
    args = parser.parse_args()

    root = args.source_root.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    test = (root / "tests/native_assert_scalar_ir_runtime_test.cpp").resolve(strict=True)
    require(test.is_file(), "missing native differential runtime test")

    with tempfile.TemporaryDirectory(prefix="oreslang-assert-ir-") as directory:
        output = Path(directory)
        ir = output / "native-assert.ll"
        with ir.open("wb") as target:
            result = subprocess.run([str(driver)], stdout=target, stderr=subprocess.PIPE,
                                    check=False, timeout=90)
        require(result.returncode == 0, "Assert IR generator failed: " +
                result.stderr.decode("utf-8", errors="replace"))
        require(0 < ir.stat().st_size <= 16 * 1024 * 1024,
                "empty or excessive Assert IR output")
        # Reject a stub that compiles no exported ABI or merely prints comments.
        source = ir.read_text(encoding="utf-8")
        require("define void @oreslang_assert_equal_scalar_v0(" in source and
                "define void @oreslang_assert_trap_bool_v0(" in source,
                "generated IR lacks actual exported function definitions")

        # LLVM 14 accepts typed pointers by default; LLVM 15+ emits opaque
        # pointers. Verify the ABI signatures themselves so a compiler-version
        # regression cannot hide behind a successful unrelated native link.
        signatures = {}
        for symbol in ("oreslang_assert_equal_scalar_v0",
                       "oreslang_assert_trap_bool_v0"):
            match = re.search(r"(?m)^define\s+void\s+@" + symbol +
                              r"\s*\(\s*([^,\n]+)", source)
            require(match is not None, "missing exported ABI signature: " + symbol)
            signatures[symbol] = match.group(1)
        if args.pointer_mode == "typed":
            require(signatures["oreslang_assert_equal_scalar_v0"].startswith(
                        "%oreslang.assert.scalar.v0*") and
                    signatures["oreslang_assert_trap_bool_v0"].startswith(
                        "%oreslang.assert.bool_result.v0*"),
                    "LLVM 14 typed pointer ABI signature regression")
        else:
            require(all(sig.startswith("ptr ") for sig in signatures.values()),
                    "LLVM 15+ opaque pointer ABI signature regression")

        binary = output / "native-assert-test"
        execute([args.clang, "-std=c++17", "-O" + args.opt,
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(root / "src/main/cpp"),
                 "-x", "ir", str(ir),
                 "-x", "c++", str(test), "-o", str(binary)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        execute([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env=env)
    print("native LLVM Assert scalar differential ABI O" + args.opt + " PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        print("native Assert IR CTest failed:", error, file=sys.stderr)
        sys.exit(1)
