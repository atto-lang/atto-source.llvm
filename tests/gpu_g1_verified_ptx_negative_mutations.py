#!/usr/bin/env python3
"""Deliberately mutate exact checked-in host code; fail if tests don't kill it.

HOST_ONLY: this is neither an AttoLang guest compiler nor GPU execution.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/main/cpp/oreslang/backend/GpuG1VerifiedPtx.hpp"
TEST = ROOT / "tests/gpu_g1_verified_ptx_test.cpp"
MUTATIONS = (
    ("disabled_digest", "if (CRYPTO_memcmp(out.data(), expected.data(), expected.size()) != 0)", "if (false)"),
    ("disabled_module_pin", "if (state_.use_count() != 1)", "if (false)"),
)

def main():
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler or not HEADER.is_file() or not TEST.is_file():
        raise SystemExit("GPU-G1-MUTATION-001: compiler or exact source/test unavailable")
    original = HEADER.read_text(encoding="utf-8")
    for name, before, after in MUTATIONS:
        if original.count(before) != 1:
            raise SystemExit(f"GPU-G1-MUTATION-002: {name} source marker missing or ambiguous")
        with tempfile.TemporaryDirectory(prefix="gpu-g1-ptx-mutation-") as temp:
            tmp = pathlib.Path(temp)
            dst = tmp / "oreslang/backend/GpuG1VerifiedPtx.hpp"
            dst.parent.mkdir(parents=True)
            dst.write_text(original.replace(before, after, 1), encoding="utf-8")
            exe = tmp / "mutant"
            cmd = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
                   "-Werror", "-UNDEBUG", "-pthread", f"-I{tmp}",
                   str(TEST), "-lcrypto", "-o", str(exe)]
            compiled = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if compiled.returncode:
                raise SystemExit(f"GPU-G1-MUTATION-003: {name} mutant failed to compile: {compiled.stderr}")
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            if result.returncode not in (-signal.SIGABRT, 128 + signal.SIGABRT):
                raise SystemExit(f"GPU-G1-MUTATION-004: {name} survived or failed unexpectedly: "
                                 f"rc={result.returncode}, stdout={result.stdout}, stderr={result.stderr}")
            print(f"GPU_G1_MUTATION_KILLED {name} (expected assertion abort)")
    print(f"PASS {len(MUTATIONS)}/{len(MUTATIONS)} host-only negative source mutations")

if __name__ == "__main__":
    main()
