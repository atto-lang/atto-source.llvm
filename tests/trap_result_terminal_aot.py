#!/usr/bin/env python3
"""Exact-source LLVM -> Clang O0/O2 checked-overflow terminal-trap proof.

This is not an ordinary-throw-to-Err/actor-supervisor certification.
"""
import argparse
import hashlib
import pathlib
import signal
import subprocess
import sys
import tempfile

def fail(reason):
    raise RuntimeError(reason)

def run(args, *, input_text=None):
    return subprocess.run(args, input=input_text, capture_output=True,
                          text=True, check=False, timeout=60)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--root", required=True)
    p.add_argument("--fixture", required=True)
    p.add_argument("--fixture-git-blob", required=True)
    args = p.parse_args()
    source = pathlib.Path(args.fixture).read_bytes()
    actual = hashlib.sha1(b"blob " + str(len(source)).encode() + b"\x00" + source).hexdigest()
    if actual != args.fixture_git_blob:
        fail("exact fixture Git blob does not match expected content")
    record = pathlib.Path(args.root) / "src/main/cpp/oreslang/runtime/NativeRecordRegion.c"
    include = pathlib.Path(args.root) / "src/main/cpp"
    if not record.is_file():
        fail("native record runtime missing")
    compiled = run([args.compiler, "--target=host", args.fixture])
    if compiled.returncode != 0 or compiled.stderr or not compiled.stdout.startswith("; ModuleID"):
        fail("compiler did not emit valid-looking LLVM module without diagnostics")
    if "llvm.sadd.with.overflow.i64" not in compiled.stdout or "llvm.trap" not in compiled.stdout:
        fail("checked overflow/trap IR was not emitted")
    with tempfile.TemporaryDirectory(prefix="atto-trap-terminal-") as tmp:
        for opt in ("0", "2"):
            exe = str(pathlib.Path(tmp) / ("terminal_O" + opt))
            link = run([args.clang, "-O" + opt, "-x", "ir", "-",
                        "-x", "c", "-std=c11", "-I", str(include),
                        str(record), "-pthread", "-o", exe],
                       input_text=compiled.stdout)
            if link.returncode != 0 or link.stderr and "error:" in link.stderr:
                fail("native LLVM/record-runtime link failed at O" + opt)
            guest = run([exe])
            if guest.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
                fail("checked overflow must terminate via SIGILL/SIGTRAP at O" + opt +
                     ", got " + str(guest.returncode))
            if guest.stdout:
                fail("terminal overflow unexpectedly printed a Result")
    print("TRAP_RESULT_TERMINAL_OVERFLOW_O0_O2_PASS")

if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print("trap Result terminal guest gate failed: " + str(error), file=sys.stderr)
        sys.exit(1)
