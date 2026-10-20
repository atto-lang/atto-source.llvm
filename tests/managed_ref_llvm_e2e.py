#!/usr/bin/env python3
"""Compile generated verified LLVM IR and execute its checked ref32 paths.

Run only with a clang compatible with the configured LLVM IR producer.
The explicit subprocess timeouts protect CI against a stuck compilation.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str, timeout: int = 75) -> subprocess.CompletedProcess:
    result = subprocess.run(args, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(
            f"Command failed: {' '.join(args)}\n"
            f"exit={result.returncode}\n"
            f"stdout:\n{result.stdout[-4000:]}\n"
            f"stderr:\n{result.stderr[-4000:]}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ir-producer", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--harness", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="oreslang-ref32-llvm-") as d:
        temp = pathlib.Path(d)
        generated = run(args.ir_producer, "--emit")
        ir = temp / "compact.ll"
        ir.write_text(generated.stdout, encoding="utf-8")
        if "ref32.authenticated" not in generated.stdout:
            raise RuntimeError("verified LLVM IR omitted original-generation proof validation")
        obj = temp / "compact.o"
        run(args.clang, "-x", "ir", "-c", str(ir), "-o", str(obj))
        exe = temp / "actor-ref32-e2e"
        run(args.clang, "-std=c++17", "-O1", "-g",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            str(args.harness), str(obj), "-o", str(exe))
        executed = run(str(exe), timeout=30)
        if "PASS LLVM compact i32 physical load/store" not in executed.stdout:
            raise RuntimeError("compiled reference contract returned no success marker")
        print(executed.stdout.strip())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as failure:
        print(f"FAIL compiled actor-ref32 LLVM execution: {failure}", file=sys.stderr)
        sys.exit(1)
