#!/usr/bin/env python3
"""Standalone host-only Symbol codec check; no LLVM SDK or Oreslang compiler.

Usage: python3 tests/run_symbol_key_codec_host.py [--sanitize] [--require-icu]
Runs the exact repository test source in ASCII-only mode, then with verified
ICU 76.x libraries when present. This is NOT Oreslang guest-code evidence.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def command(args: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="compile with ASan/UBSan in addition to standard warnings")
    parser.add_argument("--require-icu", action="store_true",
                        help="fail if ICU 76.x with pinned Unicode 16.0 is unavailable")
    options = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    source = repo / "tests" / "symbol_key_codec_v1_test.cpp"
    if not source.is_file():
        raise RuntimeError(f"missing native test source: {source}")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler or shutil.which(compiler[0]) is None:
        raise RuntimeError("C++17 compiler is required; set CXX if needed")

    packages = shutil.which("pkg-config")
    icu_version = ""
    icu_args: list[str] = []
    if packages:
        version = command(["pkg-config", "--modversion", "icu-uc"], check=False)
        if version.returncode == 0:
            icu_version = version.stdout.strip()
            if icu_version.startswith("76."):
                arguments = command(["pkg-config", "--cflags", "--libs", "icu-uc"])
                icu_args = shlex.split(arguments.stdout)
    if options.require_icu and not icu_args:
        raise RuntimeError(
            f"pinned ICU 76.x is required (found {icu_version or 'none'})")

    modes = ["ascii"] + (["icu76"] if icu_args else [])
    with tempfile.TemporaryDirectory(prefix="ores-symbol-codec-") as temp:
        for mode in modes:
            output = Path(temp) / f"symbol-v1-{mode}"
            flags = [
                "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-I", str(repo / "src" / "main" / "cpp"),
            ]
            if options.sanitize:
                flags += ["-O1", "-g", "-fsanitize=address,undefined",
                          "-fno-omit-frame-pointer"]
            if mode == "icu76":
                flags.append("-DORES_SYMBOL_V1_ICU_NFC=1")
            args = compiler + flags + [str(source)]
            if mode == "icu76":
                args += icu_args
            args += ["-o", str(output)]
            built = command(args, check=False)
            if built.returncode:
                sys.stderr.write(built.stdout + built.stderr)
                raise RuntimeError(f"{mode} native compile failed")
            executed = command([str(output)], check=False)
            if executed.returncode:
                sys.stderr.write(executed.stdout + executed.stderr)
                raise RuntimeError(f"{mode} native conformance failed")
            print(f"PASS: {mode} host-only codec test"
                  + (" (ASan/UBSan)" if options.sanitize else ""))
            if executed.stdout.strip():
                print(executed.stdout.strip())
    if not icu_args:
        print(f"ICU mode skipped (need pinned ICU 76.x; found {icu_version or 'none'}).")
    print("No assertion of LLVM guest lowering, actor transport, or Java parity.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
