#!/usr/bin/env python3
"""Compile Oreslang Boolean parameters/calls/intrinsics to native machine code.

The fixture is deliberately pure: no bool locals, ownership, imported stdlib,
suspension, actor effects or host callbacks. Every exported result is generated
by oreslang-llvmc and exercised by a separately compiled native harness.
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
    required_zero = (
        "BoolProbe.yes", "BoolProbe.no", "BoolProbe.and_word",
        "BoolProbe.and_symbol", "BoolProbe.or_word", "BoolProbe.or_symbol",
        "BoolProbe.xor_symbol", "BoolProbe.nested",
        "BoolProbe.precedence_probe",
    )
    for symbol in required_zero:
        if f"define i1 @{symbol}()" not in emitted:
            raise AssertionError(f"missing native Boolean guest export {symbol}")

    required_param = (
        "BoolProbe.and_args", "BoolProbe.or_args", "BoolProbe.xor_args",
        "BoolProbe.builtin_and", "BoolProbe.builtin_or",
        "BoolProbe.builtin_xor", "BoolProbe.forward",
        "BoolProbe.named_forward", "BoolProbe.compose",
        "BoolProbe.builtin_and8", "BoolProbe.builtin_or8",
        "BoolProbe.left_bool", "BoolProbe.right_bool",
        "BoolProbe.lazy_and_call", "BoolProbe.lazy_or_call",
        "BoolProbe.eager_xor_calls",
    )
    for symbol in required_param:
        if not re.search(rf"define i1 @{re.escape(symbol)}\(i1 ", emitted):
            raise AssertionError(f"missing i1-parameter Boolean export {symbol}")

    if "br i1" not in emitted or "phi i1" not in emitted:
        raise AssertionError("native And/Or must lower to short-circuit i1 CFG")
    if "xor i1" not in emitted:
        raise AssertionError("native Xor must lower to eager xor i1")
    if "call i1 @BoolProbe.and_args" not in emitted:
        raise AssertionError("same-module bool call did not lower to call i1")
    if re.search(r"(?m)^\s*declare\b", emitted):
        raise AssertionError("pure Boolean guest declares an external dependency")

    def body(symbol):
        marker = f"define i1 @{symbol}"
        start = emitted.find(marker)
        if start < 0:
            raise AssertionError(f"missing function body {symbol}")
        end = emitted.find("\n}", start)
        if end < 0:
            raise AssertionError(f"unterminated function body {symbol}")
        return emitted[start:end + 2]

    for symbol in ("BoolProbe.lazy_and_call", "BoolProbe.lazy_or_call"):
        block = body(symbol)
        branch = block.find("br i1")
        rhs_call = block.find("call i1 @BoolProbe.right_bool")
        if branch < 0 or rhs_call < 0 or rhs_call < branch:
            raise AssertionError(
                f"{symbol} RHS call must be emitted only after short-circuit branch")

    xor_block = body("BoolProbe.eager_xor_calls")
    left_call = xor_block.find("call i1 @BoolProbe.left_bool")
    right_call = xor_block.find("call i1 @BoolProbe.right_bool")
    xor_inst = xor_block.find("xor i1")
    if not (0 <= left_call < right_call < xor_inst):
        raise AssertionError(
            "Xor calls must execute eagerly once each in lexical left-to-right order")

    # Calls are permitted only between source-owned BoolProbe definitions.
    for line in emitted.splitlines():
        if re.search(r"\b(?:call|invoke|callbr)\b", line):
            if not re.search(r"@BoolProbe\.[A-Za-z_][A-Za-z0-9_]*\(", line):
                raise AssertionError(f"unexpected non-BoolProbe call: {line}")

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
