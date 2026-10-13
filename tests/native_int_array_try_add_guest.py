#!/usr/bin/env python3
"""Compiler-generated recoverable native Array<int>.try_add status and ownership gate.

The current guest type is the *provisional native i64 status*, not a typed
Oreslang Result. Both success and injected try_add allocation failure must
leave the owner alive until normal lexical cleanup. Test-only fault injector
and ELF --wrap instrumentation never reach the production binary.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

POSITIVE = {
    "lexical": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  const status: int = a.try_add(42);
  return status + a.size * 8;
}
""", 8, 2),
    "class": ("""define class Shelf as
  let mut Array<int> items = new Array<int>();
end
pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return shelf.items.try_add(99) + shelf.items.size * 8;
}
""", 8, 2),
    "nested": ("""fnc bump(int x): int { return x + 1; }
pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  return bump(a.try_add(7)) + a.size * 8;
}
""", 9, 3),
}

NEGATIVE = {
    "discarded-status": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.try_add(42);
  return 42;
}
""", "native Array<int> effect supports only add(value) or set(index,value)"),
    "missing-argument": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  return a.try_add();
}
""", "native Array<int> expression supports only get(index) or try_add(value)"),
    "extra-argument": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  return a.try_add(1, 2);
}
""", "native Array<int> expression supports only get(index) or try_add(value)"),
    "nonarray-receiver": ("""pub fnc main(): int {
  const x: int = 3;
  return x.try_add(99);
}
""", "Array<int> mutation requires live let mut owner"),
}

SHIM = r"""
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdlib.h>
#include <unistd.h>
static int drops = 0, closes = 0;
__attribute__((constructor)) static void inject(void) {
    const char* value = getenv("ORE_FAIL_AFTER");
    if (value) oreslang_int_array_test_fail_after(atoi(value));
}
extern void __real_oreslang_int_array_drop(OresNativeIntArrayRegion*, OresNativeIntArrayHandle);
extern void __real_oreslang_int_array_region_close(OresNativeIntArrayRegion*);
void __wrap_oreslang_int_array_drop(OresNativeIntArrayRegion* r, OresNativeIntArrayHandle h) {
    ++drops;
    __real_oreslang_int_array_drop(r, h);
}
void __wrap_oreslang_int_array_region_close(OresNativeIntArrayRegion* r) {
    ++closes;
    __real_oreslang_int_array_region_close(r);
}
__attribute__((destructor)) static void verify(void) {
    if (drops != 1 || closes != 1) _Exit(97);
}
"""

def run(args, *, env=None):
    return subprocess.run(args, capture_output=True, timeout=90, check=False, env=env)


def verify(compiler: Path, clang: Path, root: Path, sanitizers: bool):
    if sys.platform != "linux":
        raise AssertionError("requires Linux ELF linker --wrap")
    runtime = root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = root / "src/main/cpp"
    for p in (compiler, clang, runtime):
        if not p.is_file():
            raise AssertionError(f"missing input: {p}")
    with tempfile.TemporaryDirectory(prefix="ores-array-try-add-") as folder:
        wd = Path(folder)
        shim = wd / "shim.c"
        shim.write_text(SHIM, encoding="utf-8")
        for name, (source, good_code, failed_code) in POSITIVE.items():
            path = wd / f"{name}.ores"
            path.write_text(source, encoding="utf-8")
            compiled = run([str(compiler), str(path)])
            if compiled.returncode or compiled.stderr or not compiled.stdout.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler failed rc={compiled.returncode} stderr={compiled.stderr!r}")
            ir = compiled.stdout
            if (ir.count(b"call i32 @oreslang_int_array_try_add(") != 1 or
                    b"array.try_add.code" not in ir or
                    b"@oreslang_int_array_drop" not in ir or
                    b"@oreslang_int_array_region_close" not in ir):
                raise AssertionError(f"{name}: missing checked guest code/owner cleanup")
            ll = wd / f"{name}.ll"
            ll.write_bytes(ir)
            exe = wd / name
            flags = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                     "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1",
                     "-Wl,--wrap=oreslang_int_array_drop",
                     "-Wl,--wrap=oreslang_int_array_region_close"]
            if sanitizers:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            flags += ["-x", "ir", str(ll), "-x", "c", str(runtime), str(shim),
                      "-I", str(include), "-o", str(exe)]
            linked = run(flags)
            if linked.returncode:
                raise AssertionError(f"{name}: link failed: {linked.stderr!r}")
            for injected, code in ((None, good_code), ("2", failed_code)):
                env = os.environ.copy()
                env.pop("ORE_FAIL_AFTER", None)
                if injected is not None:
                    env["ORE_FAIL_AFTER"] = injected
                guest = run([str(exe)], env=env)
                if guest.returncode != code or guest.stdout or guest.stderr:
                    raise AssertionError(
                        f"{name} inject={injected}: expected {code}, "
                        f"got {guest.returncode}, out={guest.stdout!r}, err={guest.stderr!r}")
        for name, (source, message) in NEGATIVE.items():
            path = wd / f"{name}.ores"
            path.write_text(source, encoding="utf-8")
            guest = run([str(compiler), str(path)])
            if guest.returncode != 1 or guest.stdout or message.encode() not in guest.stderr:
                raise AssertionError(f"{name}: expected fail-closed '{message}', got "
                                     f"rc={guest.returncode}, stdout={guest.stdout!r}, stderr={guest.stderr!r}")
        wasm = wd / "wasm.ores"
        wasm.write_text(POSITIVE["lexical"][0], encoding="utf-8")
        rejected = run([str(compiler), "--target=wasm32", str(wasm)])
        if rejected.returncode != 1 or rejected.stdout or \
                b"wasm32 native heap-owned/Array<int> regions are not implemented" not in rejected.stderr:
            raise AssertionError("wasm32 try_add erroneously admitted")
    print("PASS source-compiled native try_add: 3 guests x (success, injected OOM), "
          "4 no-IR negatives, wasm32 rejection and exact-once cleanup"
          + (" (ASan+UBSan)" if sanitizers else ""), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.compiler.resolve(), args.clang.resolve(),
               args.source_root.resolve(), args.sanitizers)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"native try_add guest test FAILED: {error}", file=sys.stderr)
        sys.exit(1)
