#!/usr/bin/env python3
"""Native compiler execution of the first owned, lexical Array<int> source slice.

Unlike the hand-written LLVM ABI proof, every positive here starts as .ores
and is compiled by the real native Oreslang frontend + LLVM backend.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

try:
    import resource
except ImportError:
    resource = None

POSITIVE = ("native-array-source-main.ores", "native-array-source-early-return.ores")
NEGATIVE = {
    "native-array-source-readonly-negative.ores": "requires let mut owner",
    "native-array-source-escape-negative.ores": "non-Copy and cannot escape",
    "native-array-source-noalias-negative.ores": "expected 'new'",
}
ABORT = ("native-array-source-oob-negative.ores",
         "native-array-source-set-oob-negative.ores")


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, check=False, timeout=40, **kwargs)


def verify_ir(compiler, source):
    result = run([str(compiler), str(source)])
    if result.returncode != 0 or result.stderr:
        raise AssertionError(
            f"{source.name}: source compilation failed {result.returncode}, "
            f"{result.stderr!r}")
    required = (b"; ModuleID", b"@oreslang_int_array_new",
                b"@oreslang_int_array_region_close", b"@oreslang_int_array_drop")
    if not result.stdout.startswith(required[0]) or any(
            item not in result.stdout for item in required[1:]):
        raise AssertionError(f"{source.name}: missing native array allocation/cleanup IR")
    return result.stdout


def native_exe(clang, ir, runtime, include, target, sanitizers):
    command = [str(clang), "-x", "ir", "-", "-x", "c", str(runtime),
               "-std=c11", "-I", str(include), "-o", str(target)]
    if sanitizers:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    linked = run(command, input=ir)
    if linked.returncode or linked.stderr and b"error:" in linked.stderr:
        raise AssertionError(f"native link failed: {linked.stderr!r}")


def no_core_dump():
    if resource is not None:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def execute(exe, expected, sanitizers=False):
    env = os.environ.copy()
    if sanitizers:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    result = run([str(exe)], env=env,
                 preexec_fn=no_core_dump if resource is not None else None)
    if result.returncode != expected:
        raise AssertionError(
            f"expected process exit {expected}, got {result.returncode}: "
            f"stderr={result.stderr[:2000]!r}")
    if expected == 42 and result.stderr:
        raise AssertionError(f"unexpected stderr from positive native guest: {result.stderr[:2000]!r}")


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--compiler", type=Path, required=True)
    args.add_argument("--clang", type=Path, required=True)
    args.add_argument("--fixture-root", type=Path, required=True)
    args.add_argument("--source-root", type=Path, required=True)
    args.add_argument("--sanitize", action="store_true")
    ns = args.parse_args()
    compiler, clang = ns.compiler.resolve(), ns.clang.resolve()
    runtime = ns.source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = ns.source_root / "src/main/cpp"
    if not compiler.is_file() or not clang.is_file() or not runtime.is_file():
        raise AssertionError("native compiler, clang, or runtime missing")
    with tempfile.TemporaryDirectory(prefix="oreslang-array-source-") as temp:
        executable = Path(temp) / "native-array-guest"
        for filename in POSITIVE:
            source = ns.fixture_root / filename
            native_exe(clang, verify_ir(compiler, source), runtime, include,
                       executable, ns.sanitize)
            execute(executable, 42, ns.sanitize)
            print("PASS: compiler-generated Array<int> native execution", filename,
                  "sanitizers" if ns.sanitize else "normal", flush=True)
        if ns.sanitize:
            return
        for filename, expected in NEGATIVE.items():
            response = run([str(compiler), str(ns.fixture_root / filename)])
            if response.returncode != 1 or response.stdout or \
                    expected.encode("utf-8") not in response.stderr:
                raise AssertionError(
                    f"compiler must fail closed for {filename}: {response!r}")
            print("PASS: compiler rejects unsupported ownership:", filename, flush=True)
        wasm = run([str(compiler), "--target=wasm32",
                    str(ns.fixture_root / POSITIVE[0])])
        if wasm.returncode != 1 or wasm.stdout or \
                b"wasm32 native heap-owned regions" not in wasm.stderr:
            raise AssertionError(f"unsafe wasm32 admission: {wasm!r}")
        if sys.platform != "win32":
            for filename in ABORT:
                native_exe(clang, verify_ir(compiler, ns.fixture_root / filename),
                           runtime, include, executable, False)
                execute(executable, -signal.SIGABRT)
                print("PASS: native Array<int> bounds failure aborts:", filename,
                      flush=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print("native Array<int> source runtime failure:", error, file=sys.stderr)
        sys.exit(1)
