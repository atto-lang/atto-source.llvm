#!/usr/bin/env python3
"""Conservative IR evidence preflight: deny unproven alias/vector promises.

This does not show whether LLVM *automatically* vectorizes a function.
"""
import re
import sys
from pathlib import Path

UNPROVEN = re.compile(r"!(?:tbaa(?:\.struct)?|alias\.scope|noalias|llvm\.loop|llvm\.access\.group|llvm\.mem\.parallel_loop_access|invariant\.(?:load|group))\b|\bnoalias\b")
# Reject all LLVM fast-math flags, including compound flags and attributes.
# This scanner is a conservative textual audit, not a complete LLVM parser.
FAST_MATH = re.compile(r"\b(?:fneg|fadd|fsub|fmul|fdiv|frem|fcmp|select|phi|call|invoke)\s+(?:fast|nnan|ninf|nsz|arcp|contract|afn|reassoc)\b")


class UnsafeOptimizationMetadata(ValueError):
    pass


def inspect(ir):
    if not isinstance(ir, str):
        raise TypeError("IR must be a UTF-8 text string")
    if len(ir.encode("utf-8")) > 1024 * 1024:
        raise UnsafeOptimizationMetadata("IR exceeds bounded audit size")
    findings = []
    for line_number, raw in enumerate(ir.splitlines(), 1):
        # LLVM ';' comments are never optimization evidence.
        line = raw.split(";", 1)[0]
        if UNPROVEN.search(line) or re.search(r"\b(?:nonnull|dereferenceable(?:_or_null)?|nocapture|readonly|writeonly|align)\b", line) and re.search(r"\b(?:define|declare|call|invoke)\b", line):
            findings.append(f"line {line_number}: alias/loop metadata without ownership proof")
        if FAST_MATH.search(line):
            findings.append(f"line {line_number}: floating-point fast-math requires explicit semantics")
    if findings:
        raise UnsafeOptimizationMetadata("\n".join(findings))
    return {"status": "UNPROVEN_OPTIMIZATIONS_ABSENT", "lines_checked": len(ir.splitlines()),
            "actual_vectorization_verified": False, "alias_disjointness_verified": False}


def main(args):
    if len(args) != 2:
        print("usage: verify_ownership_optimization_ir.py program.ll", file=sys.stderr)
        return 64
    try:
        path = Path(args[1])
        if path.stat().st_size > 1024 * 1024:
            raise UnsafeOptimizationMetadata("IR exceeds bounded audit size")
        report = inspect(path.read_text(encoding="utf8"))
        print(f"{report['status']}: {report['lines_checked']} IR lines inspected")
        return 0
    except (OSError, UnicodeError, UnsafeOptimizationMetadata) as exc:
        print(f"REJECTED: {exc}", file=sys.stderr)
        return 65


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
