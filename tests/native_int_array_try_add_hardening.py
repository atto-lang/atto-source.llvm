#!/usr/bin/env python3
"""Adversarial guest-native checked Array<int> status and state preservation.

Real Oreslang compiler -> LLVM IR -> Clang + real trusted C ABI. Test-only
ELF linker wrappers synthesize an invalid ABI status and force genuine quota
exhaustion. An unrelated case tests allocation OOM after four committed
values, with owner drop and region close verified on every exit.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PROGRAMS = {
    "unknown": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  return a.try_add(42);
}
""", "BAD", 86),
    "quota": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  const status: int = a.try_add(42);
  return status + a.get(65535);
}
""", "QUOTA", 8),
    "populated": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.add(10);
  a.add(11);
  a.add(12);
  a.add(13);
  return a.try_add(42) + a.size * 8 + a.get(0);
}
""", "OOM", 44),
    "populated_success": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.add(10);
  a.add(11);
  a.add(12);
  a.add(13);
  return a.try_add(42) + a.size * 8 + a.get(0);
}
""", "NONE", 50),
}
SHIM = r"""
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#include <stdlib.h>
static int drops = 0;
static int closes = 0;
__attribute__((constructor)) static void initialize(void) {
    if (getenv("ORE_OOM"))
        oreslang_int_array_test_fail_after(3);
}
extern OresNativeIntArrayStatus __real_oreslang_int_array_try_add(
    OresNativeIntArrayRegion*, OresNativeIntArrayHandle, int64_t);
OresNativeIntArrayStatus __wrap_oreslang_int_array_try_add(
        OresNativeIntArrayRegion* r, OresNativeIntArrayHandle h, int64_t v) {
    if (getenv("ORE_BAD")) return (OresNativeIntArrayStatus)99;
    if (getenv("ORE_QUOTA")) {
        if (oreslang_int_array_length(r, h) != 0)
            _Exit(91);
        if (oreslang_int_array_try_reserve(r, h, 65536) != ORES_NATIVE_ARRAY_OK)
            _Exit(92);
        for (uint64_t i = 0; i < 65536; ++i)
            if (__real_oreslang_int_array_try_add(r, h, 7) != ORES_NATIVE_ARRAY_OK)
                _Exit(93);
        OresNativeIntArrayStatus status =
            __real_oreslang_int_array_try_add(r, h, v);
        if (status != ORES_NATIVE_ARRAY_LIMIT ||
            oreslang_int_array_length(r, h) != 65536 ||
            oreslang_int_array_get(r, h, 65535) != 7)
            _Exit(94);
        return status;
    }
    return __real_oreslang_int_array_try_add(r, h, v);
}
extern void __real_oreslang_int_array_drop(
    OresNativeIntArrayRegion*, OresNativeIntArrayHandle);
extern void __real_oreslang_int_array_region_close(
    OresNativeIntArrayRegion*);
void __wrap_oreslang_int_array_drop(
        OresNativeIntArrayRegion* r, OresNativeIntArrayHandle h) {
    ++drops;
    __real_oreslang_int_array_drop(r, h);
}
void __wrap_oreslang_int_array_region_close(OresNativeIntArrayRegion* r) {
    ++closes;
    __real_oreslang_int_array_region_close(r);
}
__attribute__((noreturn)) void __wrap_abort(void) {
    _Exit(drops == 1 && closes == 1 ? 86 : 87);
}
__attribute__((destructor)) static void verify_normal_cleanup(void) {
    if (drops != 1 || closes != 1) _Exit(88);
}
"""

def execute(args, env=None):
    return subprocess.run(args, capture_output=True, timeout=90, check=False, env=env)

def verify(compiler, clang, source_root, sanitizers):
    if sys.platform != "linux":
        raise AssertionError("Linux ELF linker --wrap required")
    runtime = source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    for f in (compiler, clang, runtime):
        if not f.is_file():
            raise AssertionError(f"missing input {f}")
    with tempfile.TemporaryDirectory(prefix="oreslang-checked-guest-") as temp:
        directory = Path(temp)
        shim = directory / "shim.c"
        shim.write_text(SHIM, encoding="utf-8")
        for name, (source, injection, result) in PROGRAMS.items():
            source_path = directory / (name + ".ores")
            source_path.write_text(source, encoding="utf-8")
            compiled = execute([str(compiler), str(source_path)])
            if compiled.returncode or compiled.stderr or not compiled.stdout:
                raise AssertionError(f"{name}: compiler failure {compiled.stderr!r}")
            ir = compiled.stdout
            if b"@oreslang_int_array_try_add" not in ir or \
                    b"array.try_add.known.status" not in ir or \
                    b"array.resource.failure" not in ir:
                raise AssertionError(f"{name}: missing checked status or cleanup blocks")
            path = directory / (name + ".ll")
            path.write_bytes(ir)
            exe = directory / name
            flags = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                     "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1",
                     "-Wl,--wrap=oreslang_int_array_try_add",
                     "-Wl,--wrap=oreslang_int_array_drop",
                     "-Wl,--wrap=oreslang_int_array_region_close",
                     "-Wl,--wrap=abort"]
            if sanitizers:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            flags += ["-x", "ir", str(path), "-x", "c", str(runtime), str(shim),
                      "-I", str(source_root / "src/main/cpp"), "-o", str(exe)]
            linked = execute(flags)
            if linked.returncode:
                raise AssertionError(f"{name}: link failure: {linked.stderr!r}")
            env = os.environ.copy()
            for key in ("ORE_BAD", "ORE_QUOTA", "ORE_OOM"):
                env.pop(key, None)
            if injection != "NONE":
                env["ORE_" + injection] = "1"
            observed = execute([str(exe)], env=env)
            if observed.returncode != result or observed.stdout or observed.stderr:
                raise AssertionError(
                    f"{name}: expected {result}, got {observed.returncode}, "
                    f"stdout={observed.stdout!r}, stderr={observed.stderr!r}")
    print("PASS: 4 source-compiled guests: invalid ABI fail-closed, true quota, "
          "populated OOM rollback, normal append; verified exact owner cleanup"
          + (" (ASan+UBSan)" if sanitizers else ""), flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.compiler.resolve(), args.clang.resolve(),
               args.source_root.resolve(), args.sanitizers)
    except (OSError, AssertionError, subprocess.TimeoutExpired) as exc:
        print("try_add hardening test FAILED:", exc, file=sys.stderr)
        sys.exit(1)
