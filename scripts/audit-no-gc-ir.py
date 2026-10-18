#!/usr/bin/env python3
"""Fail closed on known tracing-GC LLVM IR in the standalone native AOT path.

This detects forbidden *IR constructs*, not the complete transitive linked
runtime, dlopen/FFI plugin closure or absence of every possible collector.
A full zero-GC release requires separate linked-image/runtime evidence.
"""
import re
import sys
from pathlib import Path

# LLVM GC strategy annotations, gc.statepoint relocation family and legacy
# root/read/write intrinsics. A source-level regex cannot prove entire memory
# safety, but must not admit known tracing-GC IR as a native AOT artifact.
GC_STRATEGY = re.compile(r'\bgc\s+"(?:[^"\\]|\\.)*"')
GC_INTRINSIC = re.compile(
    r'@(?:"|)(?:llvm\.gcroot|llvm\.gcread|llvm\.gcwrite|'
    r'llvm\.gc\.(?:statepoint|relocate|result)|llvm\.experimental\.gc\.)'
)
GC_RUNTIME = re.compile(
    r'@(?:"|)(?:GC_(?:malloc|malloc_atomic|realloc|gcollect|enable|disable)|'
    r'mono_gc_|g_gc_|jl_gc_collect|PyGC_Collect|'
    r'gc_(?:collect|alloc|mark|sweep|root))'
)
MAX_IR_BYTES = 64 * 1024 * 1024


def audit_ir(text):
    for line_number, raw in enumerate(text.splitlines(), 1):
        # An IR comment may contain literal examples and is not an instruction.
        # The compiler currently emits no embedded ';' in symbol names; if it
        # ever does, use an LLVM parser instead of loosening this admission.
        line = raw.split(";", 1)[0]
        if GC_STRATEGY.search(line):
            raise ValueError(f"line {line_number}: tracing GC strategy forbidden")
        if GC_INTRINSIC.search(line):
            raise ValueError(f"line {line_number}: LLVM GC intrinsic forbidden")
        if GC_RUNTIME.search(line):
            raise ValueError(f"line {line_number}: known tracing collector call forbidden")


def main(argv):
    if len(argv) != 2:
        print("usage: audit-no-gc-ir.py program.ll", file=sys.stderr)
        return 64
    try:
        path = Path(argv[1])
        if path.stat().st_size > MAX_IR_BYTES:
            raise ValueError("IR input exceeds 64 MiB gate limit")
        audit_ir(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        print("STRICT_NO_GC_IR_REJECTED: " + str(error), file=sys.stderr)
        return 1
    print("STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
