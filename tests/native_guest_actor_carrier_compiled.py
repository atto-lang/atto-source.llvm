#!/usr/bin/env python3
"""Compile .atto recursion into a shared image and execute it on one real
MultiActorCarrier worker via a C++ host adapter, followed by a sibling actor.

NOT source-compiled AttoLang actor declarations or supervisor recovery.
"""
import argparse
import hashlib
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

def checked(command, *, timeout=75, low_stack=False):
    preexec = None
    if low_stack:
        def limit_stack():
            soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
            cap = 1024 * 1024
            if hard != resource.RLIM_INFINITY:
                cap = min(cap, hard)
            resource.setrlimit(resource.RLIMIT_STACK, (cap, hard))
        preexec = limit_stack
    outcome = subprocess.run(
        command, capture_output=True, stdin=subprocess.DEVNULL,
        timeout=timeout, check=False, preexec_fn=preexec)
    if outcome.returncode != 0:
        raise RuntimeError(
            "command failed (" + str(outcome.returncode) + "): "
            + repr(command) + " stderr=" + repr(outcome.stderr[:1200]))
    return outcome

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    ap.add_argument("--cxx", type=Path, required=True)
    args = ap.parse_args()
    for tool in (args.compiler, args.clang, args.cxx):
        if not tool.is_file():
            raise RuntimeError("real LLVM compiler, Clang and C++ toolchain required")

    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="atto-native-actor-guest-m1-") as tmp:
        directory = Path(tmp)
        source = directory / "recursive.atto"
        source.write_text(
            "fnc count_down(int n): int { return 1 + count_down(n - 1); }\n"
            "pub fnc main(): int { return loop(100000); }\n",
            encoding="utf-8")
        compiler = str(args.compiler.resolve())
        clang = str(args.clang.resolve())
        cxx = str(args.cxx.resolve())
        ir_result = checked([compiler, str(source)])
        if b"__atto_guest_exhausted" not in ir_result.stdout or b"define i32 @main(" not in ir_result.stdout:
            raise RuntimeError("real compiled guest frame ABI not present")
        ir = directory / "guest.ll"
        ir.write_bytes(ir_result.stdout)
        image = directory / "libatto_guest.so"
        checked([clang, "-O0", "-shared", "-fPIC", "-x", "ir", str(ir), "-o", str(image)])
        if not image.is_file():
            raise RuntimeError("native guest dynamic library was not created")
        test = root / "tests/native_guest_actor_carrier_compiled_test.cpp"
        runtime = root / "src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp"
        executable = directory / "carrier_guest"
        checked([
            cxx, "-std=c++17", "-O0", "-pthread",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I", str(root / "src/main/cpp"),
            str(test), str(runtime), "-ldl", "-o", str(executable),
        ])
        observed = checked([str(executable), str(image)], low_stack=True)
        if observed.stdout != b"ATTO_SOURCE_COMPILED_GUEST_ON_SHARED_NATIVE_CARRIER_OK\n" or observed.stderr:
            raise RuntimeError("shared-carrier guest acceptance receipt mismatched")
        print("PASS compiled .atto recursive guest + native actor host adapter",
              "carrier_stack_limit=1MiB",
              "source_sha256=" + hashlib.sha256(source.read_bytes()).hexdigest(),
              "llvm_ir_sha256=" + hashlib.sha256(ir_result.stdout).hexdigest(),
              "guest_image_sha256=" + hashlib.sha256(image.read_bytes()).hexdigest(),
              "carrier_image_sha256=" + hashlib.sha256(executable.read_bytes()).hexdigest(),
              "compiler_sha256=" + hashlib.sha256(args.compiler.read_bytes()).hexdigest())
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("AttoLang compiler-guest carrier proof NOT_VERIFIED:", error, file=sys.stderr)
        sys.exit(1)
