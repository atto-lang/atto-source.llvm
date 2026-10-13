#!/usr/bin/env python3
"""Source-compiled repeated fallible append: same-owner recovery and exact drops.

The first failure is a genuine native allocation failure, not a forged status.
Other cases deliberately deny a checked call without modifying its array.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = """pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.add(10);
  a.add(11);
  a.add(12);
  a.add(13);
  return a.try_add(20) * 40 + a.try_add(21) * 20
       + a.size * 4 + a.get(0) + a.get(3);
}
"""

# Exit code = first_status*40 + second_status*20 + length*4 + 10 + 13.
CASES = {
    "both-success": (47, (0, 0)),
    "real-oom-first": (123, (2, 0)),
    "limit-first": (83, (1, 0)),
    "limit-second": (63, (0, 1)),
    "both-limit": (99, (1, 1)),
    "oom-then-limit": (139, (2, 1)),
}

SHIM = r"""
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int drops = 0, closes = 0, checked_calls = 0;
static uint32_t outcomes[2] = {0, 0};
static const char* scenario(void) {
    const char* s = getenv("ORE_RETRY_CASE");
    if (!s) _Exit(90);
    return s;
}
extern OresNativeIntArrayStatus __real_oreslang_int_array_try_add(
    OresNativeIntArrayRegion*, OresNativeIntArrayHandle, int64_t);
OresNativeIntArrayStatus __wrap_oreslang_int_array_try_add(
        OresNativeIntArrayRegion* r, OresNativeIntArrayHandle h, int64_t v) {
    if (v != 20 && v != 21)
        return __real_oreslang_int_array_try_add(r, h, v);
    const char* mode = scenario();
    if (checked_calls >= 2 || (checked_calls == 0 && v != 20)
            || (checked_calls == 1 && v != 21)) _Exit(91);
    const uint64_t old_length = oreslang_int_array_length(r, h);
    if (old_length < 4 || old_length > 5
            || oreslang_int_array_get(r, h, 0) != 10
            || oreslang_int_array_get(r, h, 3) != 13) _Exit(92);
    OresNativeIntArrayStatus status;
    const int first = checked_calls == 0;
    const int force_oom = first
            && (!strcmp(mode, "real-oom-first") || !strcmp(mode, "oom-then-limit"));
    const int force_limit = (first && (!strcmp(mode, "limit-first")
            || !strcmp(mode, "both-limit")))
            || (!first && (!strcmp(mode, "limit-second")
            || !strcmp(mode, "both-limit") || !strcmp(mode, "oom-then-limit")));
    if (force_limit) {
        status = ORES_NATIVE_ARRAY_LIMIT;
    } else {
        if (force_oom) oreslang_int_array_test_fail_after(0);
        status = __real_oreslang_int_array_try_add(r, h, v);
        if (force_oom) oreslang_int_array_test_fail_after(-1);
        if (force_oom && status != ORES_NATIVE_ARRAY_NO_MEMORY) _Exit(93);
    }
    const uint64_t new_length = oreslang_int_array_length(r, h);
    if (new_length != old_length + (status == ORES_NATIVE_ARRAY_OK)) _Exit(94);
    if (status == ORES_NATIVE_ARRAY_OK
            && oreslang_int_array_get(r, h, old_length) != v) _Exit(95);
    if (oreslang_int_array_get(r, h, 0) != 10
            || oreslang_int_array_get(r, h, 3) != 13) _Exit(96);
    outcomes[checked_calls++] = (uint32_t)status;
    return status;
}
extern void __real_oreslang_int_array_drop(
    OresNativeIntArrayRegion*, OresNativeIntArrayHandle);
extern void __real_oreslang_int_array_region_close(OresNativeIntArrayRegion*);
void __wrap_oreslang_int_array_drop(
        OresNativeIntArrayRegion* r, OresNativeIntArrayHandle h) {
    if (checked_calls != 2 || ++drops != 1) _Exit(97);
    const uint64_t length = oreslang_int_array_length(r, h);
    if (length != 4 + (outcomes[0] == 0) + (outcomes[1] == 0)) _Exit(98);
    for (uint64_t i = 0; i < 4; ++i)
        if (oreslang_int_array_get(r, h, i) != (int64_t)(10 + i)) _Exit(99);
    uint64_t cursor = 4;
    if (outcomes[0] == 0 && oreslang_int_array_get(r, h, cursor++) != 20) _Exit(100);
    if (outcomes[1] == 0 && oreslang_int_array_get(r, h, cursor++) != 21) _Exit(101);
    __real_oreslang_int_array_drop(r, h);
}
void __wrap_oreslang_int_array_region_close(OresNativeIntArrayRegion* r) {
    if (drops != 1 || ++closes != 1) _Exit(102);
    __real_oreslang_int_array_region_close(r);
}
__attribute__((destructor)) static void verify_cleanup(void) {
    if (checked_calls != 2 || drops != 1 || closes != 1) _Exit(103);
}
"""

def run(argv, env=None):
    return subprocess.run(argv, env=env, capture_output=True,
                          timeout=90, check=False)

def verify(compiler: Path, clang: Path, root: Path, sanitizers: bool):
    if sys.platform != "linux":
        raise AssertionError("ELF --wrap is required")
    runtime = root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    for item in (compiler, clang, runtime):
        if not item.is_file():
            raise AssertionError(f"missing {item}")
    with tempfile.TemporaryDirectory(prefix="ores-recoverable-retry-") as folder:
        wd = Path(folder)
        source = wd / "retry.ores"
        source.write_text(SOURCE, encoding="utf-8")
        built = run([str(compiler), str(source)])
        if built.returncode != 0 or built.stderr or not built.stdout.startswith(b"; ModuleID"):
            raise AssertionError(f"compiler failed: {built.stderr!r}")
        if (built.stdout.count(b"call i32 @oreslang_int_array_try_add(") != 6
                or built.stdout.count(b"array.try_add.known.status") != 2):
            # Four ordinary array.add() effects + two checked expressions.
            raise AssertionError("missing two closed-domain checked statuses")
        ll = wd / "retry.ll"
        ll.write_bytes(built.stdout)
        shim = wd / "retry_shim.c"
        shim.write_text(SHIM, encoding="utf-8")
        exe = wd / "retry"
        args = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1",
                "-Wl,--wrap=oreslang_int_array_try_add",
                "-Wl,--wrap=oreslang_int_array_drop",
                "-Wl,--wrap=oreslang_int_array_region_close"]
        if sanitizers:
            args += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        args += ["-x", "ir", str(ll), "-x", "c", str(runtime), str(shim),
                 "-I", str(root / "src/main/cpp"), "-o", str(exe)]
        linked = run(args)
        if linked.returncode:
            raise AssertionError(f"Clang linking failed: {linked.stderr!r}")
        for name, (code, _) in CASES.items():
            env = os.environ.copy()
            env["ORE_RETRY_CASE"] = name
            result = run([str(exe)], env=env)
            if result.returncode != code or result.stdout or result.stderr:
                raise AssertionError(f"{name}: expected {code}, got "
                                     f"{result.returncode} stdout={result.stdout!r} "
                                     f"stderr={result.stderr!r}")
    print("PASS 6 source-compiled same-owner try_add retry cases, genuine "
          "NO_MEMORY, LIMIT, exact contents and exactly-once cleanup"
          + (" (ASan+UBSan)" if sanitizers else ""), flush=True)

if __name__ == "__main__":
    cli = argparse.ArgumentParser()
    cli.add_argument("--compiler", type=Path, required=True)
    cli.add_argument("--clang", type=Path, required=True)
    cli.add_argument("--source-root", type=Path, required=True)
    cli.add_argument("--sanitizers", action="store_true")
    args = cli.parse_args()
    try:
        verify(args.compiler.resolve(), args.clang.resolve(),
               args.source_root.resolve(), args.sanitizers)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"recoverable checked retry FAILED: {error}", file=sys.stderr)
        sys.exit(1)
