#!/usr/bin/env python3
"""Exact native compiler admission + shallow .atto process execution proof.

This does NOT prove actor-local recoverable stack exhaustion: recursive
programs are deliberately rejected before LLVM IR emission in M0.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

CASES = {
    "shallow": ("pub fnc main(): int { return 42; }\n", True),
    "self_non_tail": ("fnc descend(int n): int { return 1 + descend(n - 1); }\n"
                       "pub fnc main(): int { return descend(100000); }\n", False),
    "mutual_non_tail": ("fnc left(int n): int { return 1 + right(n - 1); }\n"
                         "fnc right(int n): int { return 1 + left(n - 1); }\n"
                         "pub fnc main(): int { return left(100000); }\n", False),
    "self_tail_pending": ("fnc down(int n): int { return down(n - 1); }\n"
                           "pub fnc main(): int { return down(100000); }\n", False),
}

def run(argv, timeout=20):
    return subprocess.run(argv, capture_output=True, stdin=subprocess.DEVNULL,
                          timeout=timeout, check=False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    args = ap.parse_args()
    for tool in (args.compiler, args.clang):
        if not tool.is_file():
            raise RuntimeError("compiler/Clang required; not an optional skip")
    with tempfile.TemporaryDirectory(prefix="atto-llvm-stack-m0-") as work:
        root = Path(work)
        for label, (source, should_compile) in CASES.items():
            src = root / (label + ".atto")
            src.write_text(source, encoding="utf-8")
            compiled = run([str(args.compiler.resolve()), str(src)])
            if should_compile:
                if compiled.returncode != 0 or b"define i32 @main(" not in compiled.stdout:
                    raise RuntimeError(label + ": actual native compiler did not emit OS entry")
                ir = root / (label + ".ll")
                ir.write_bytes(compiled.stdout)
                exe = root / label
                linked = run([str(args.clang.resolve()), "-O0", "-x", "ir",
                              str(ir), "-o", str(exe)])
                if linked.returncode or not exe.is_file():
                    raise RuntimeError(label + ": actual Clang link failed")
                observed = run([str(exe.resolve())])
                if observed.returncode != 42 or observed.stdout or observed.stderr:
                    raise RuntimeError(label + ": native guest did not return expected 42")
                print("PASS compiler-backed shallow .atto native process",
                      hashlib.sha256(compiled.stdout).hexdigest())
            else:
                if compiled.returncode == 0 or compiled.stdout:
                    raise RuntimeError(label + ": recursive source emitted native LLVM IR")
                if b"native recursive call graph requires bounded guest frames" not in compiled.stderr:
                    raise RuntimeError(label + ": failure was not the expected bounded-frame admission")
                print("PASS fail-closed recursive .atto admission", label)
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("AttoLang native stack admission NOT_VERIFIED:", error, file=sys.stderr)
        sys.exit(1)
