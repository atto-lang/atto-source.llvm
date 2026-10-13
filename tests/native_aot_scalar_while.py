#!/usr/bin/env python3
"""Compile real .ores to LLVM IR and execute its compiler-generated resume ABI."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--source-root", required=True)
    args = ap.parse_args()
    root = Path(args.source_root).resolve()
    source = root / "tests/fixtures/native-rt-aot-counter.ores"
    negative = root / "tests/fixtures/native-rt-aot-bad-update.ores"
    program = subprocess.run([args.compiler, str(source)], check=False,
                             capture_output=True, text=True, timeout=20)
    assert program.returncode == 0, program.stderr
    assert "define i32 @oreslang_rt_spin_resume(" in program.stdout, program.stdout
    assert "define i32 @oreslang_rt_spin_init(" in program.stdout, program.stdout
    assert "define i32 @oreslang_rt_spin_cancel(" in program.stdout, program.stdout
    assert "define i32 @main(" not in program.stdout, "invented an OS main()"
    assert "oreslang_source_actor_" not in program.stdout, "faked actor ABI"

    rejected = subprocess.run([args.compiler, str(negative)], check=False,
                              capture_output=True, text=True, timeout=20)
    assert rejected.returncode != 0, "unsafe source unexpectedly compiled"
    assert "first AOT continuation supports only increment by one" in rejected.stderr
    assert not rejected.stdout.strip(), "partial IR emitted after rejection"

    with tempfile.TemporaryDirectory(prefix="oreslang-aot-rt-") as directory:
        tmp = Path(directory)
        ir = tmp / "guest.ll"
        obj = tmp / "guest.o"
        exe = tmp / "guest"
        ir.write_text(program.stdout)
        llvm = subprocess.run([args.clang, "-O3", "-x", "ir", "-c", str(ir),
                               "-o", str(obj)], check=False,
                              capture_output=True, text=True, timeout=30)
        assert llvm.returncode == 0, llvm.stderr
        linked = subprocess.run([args.cxx, "-std=c++17", "-O3", "-DNDEBUG",
                                 "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                 "-I", str(root / "src/main/cpp"),
                                 str(root / "tests/native_aot_scalar_while_harness.cpp"),
                                 str(obj), "-o", str(exe)], check=False,
                                capture_output=True, text=True, timeout=30)
        assert linked.returncode == 0, linked.stderr
        executed = subprocess.run([str(exe)], check=False,
                                  capture_output=True, text=True, timeout=10)
        assert executed.returncode == 0, executed.stderr
        assert "AOT_SOURCE_SCALAR_LOOP_RESUMPTION_PASS" in executed.stdout


if __name__ == "__main__":
    main()
