#!/usr/bin/env python3
"""End-to-end *guest compiler-generated* checked native Array<int> cleanup.

The test compiles actual .ores through oreslang-llvmc and Clang, injects all
resource failure edges, and wraps the C ABI at the ELF link boundary. Exactly
the created owner handles must be dropped and the private region closed before
the compiler's terminal abort. This is NOT guest-visible throw/Result support.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

BATCH_COMMIT = """define class Four as
  let mut Array<int> previous = new Array<int>();
  let mut Array<int> following = new Array<int>();
  let mut Array<int> values = new Array<int>();
  let mut Array<int> occupied = new Array<int>();
end
pub fnc main(): int {
  let mut Four q = new Four();
  rt array_commit_filled(q, 7, -1, -1, 7, 35);
  return q.values.get(6) + q.occupied.get(0);
}
"""
LEXICAL = """pub fnc main(): int {
  let mut Array<int> first = new Array<int>();
  let mut Array<int> second = new Array<int>();
  first.add(7);
  second.add(35);
  return first.get(0) + second.get(0);
}
"""
REPLACED_FIELD = """define class Pair as
  let mut Array<int> first = new Array<int>();
  let mut Array<int> second = new Array<int>();
end
pub fnc main(): int {
  let mut Pair pair = new Pair();
  let mut Array<int> staged = new Array<int>();
  pair.first = rt take(staged);
  pair.first.add(7);
  pair.second.add(35);
  return pair.first.get(0) + pair.second.get(0);
}
"""
MOVED_CLASS = """define class Pair as
  let mut Array<int> first = new Array<int>();
  let mut Array<int> second = new Array<int>();
end
pub fnc main(): int {
  let mut Pair original = new Pair();
  let mut Pair moved = rt take(original);
  moved.first.add(7);
  moved.second.add(35);
  return moved.first.get(0) + moved.second.get(0);
}
"""
MIXED = """pub fnc main(): int {
  const box: owned int = 5;
  let mut Array<int> a = new Array<int>();
  a.add(37);
  return box + a.get(0);
}
"""
CLASS = """define class Four as
  let mut Array<int> a = new Array<int>();
  let mut Array<int> b = new Array<int>();
  let mut Array<int> c = new Array<int>();
  let mut Array<int> d = new Array<int>();
end
pub fnc main(): int {
  let mut Four f = new Four();
  f.a.add(10);
  f.b.add(11);
  f.c.add(12);
  f.d.add(9);
  return f.a.get(0) + f.b.get(0) + f.c.get(0) + f.d.get(0);
}
"""
# Counter instrumentation is entirely test-only: --wrap exists in the ELF
# linker, and no wrapper or fault injector is included in production binaries.
C_WRAPPERS = r"""
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int dropped = 0;
static int closed = 0;
static int owned_dropped = 0;
static int owned_closed = 0;
static int expectations(void) {
    const char* wanted_drops = getenv("ORE_EXPECT_DROPS");
    const char* wanted_closes = getenv("ORE_EXPECT_CLOSES");
    const char* wanted_owned_drops = getenv("ORE_EXPECT_OWNED_DROPS");
    const char* wanted_owned_closes = getenv("ORE_EXPECT_OWNED_CLOSES");
    if (!wanted_drops || !wanted_closes || !wanted_owned_drops ||
        !wanted_owned_closes) return 0;
    return dropped == atoi(wanted_drops) && closed == atoi(wanted_closes) &&
        owned_dropped == atoi(wanted_owned_drops) &&
        owned_closed == atoi(wanted_owned_closes);
}
__attribute__((constructor)) static void setup(void) {
    const char* injected = getenv("ORE_FAIL_AFTER");
    if (!injected) _Exit(97);
    oreslang_int_array_test_fail_after(atoi(injected));
}
extern void __real_oreslang_int_array_drop(
    OresNativeIntArrayRegion*, OresNativeIntArrayHandle);
extern void __real_oreslang_int_array_region_close(OresNativeIntArrayRegion*);
void __wrap_oreslang_int_array_drop(OresNativeIntArrayRegion* r,
                                   OresNativeIntArrayHandle h) {
    ++dropped;
    __real_oreslang_int_array_drop(r, h);
}
void __wrap_oreslang_int_array_region_close(OresNativeIntArrayRegion* r) {
    // Only count a close that actually completed. An invalid live-region
    // close aborts in the real runtime and must never pass this assertion.
    __real_oreslang_int_array_region_close(r);
    ++closed;
}
extern void __real_oreslang_owned_i64_region_drop(
    OresOwnedI64Region*, OresOwnedI64Handle);
extern void __real_oreslang_owned_i64_region_close(OresOwnedI64Region*);
void __wrap_oreslang_owned_i64_region_drop(OresOwnedI64Region* r,
                                          OresOwnedI64Handle h) {
    ++owned_dropped;
    __real_oreslang_owned_i64_region_drop(r, h);
}
void __wrap_oreslang_owned_i64_region_close(OresOwnedI64Region* r) {
    __real_oreslang_owned_i64_region_close(r);
    ++owned_closed;
}
__attribute__((noreturn)) void __wrap_abort(void) {
    if (expectations()) {
        (void)write(2, "CHECKED_ABORT_OK\n", 17);
        _Exit(86);
    }
    (void)fprintf(stderr, "BAD_CLEANUP dropped=%d closed=%d owned_dropped=%d owned_closed=%d\n",
                      dropped, closed, owned_dropped, owned_closed);
    _Exit(87);
}
__attribute__((destructor)) static void verify_normal_exit(void) {
    if (!expectations()) {
        (void)fprintf(stderr, "BAD_NORMAL_CLEANUP dropped=%d closed=%d owned_dropped=%d owned_closed=%d\n",
                      dropped, closed, owned_dropped, owned_closed);
        _Exit(88);
    }
}
"""


def run(args, *, env=None):
    return subprocess.run(args, capture_output=True, timeout=90, check=False, env=env)


def verify(compiler, clang, root, sanitizers):
    if sys.platform != "linux":
        raise AssertionError("checked cleanup test requires ELF ld --wrap on Linux")
    runtime = root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    for p in (compiler, clang, runtime):
        if not p.is_file():
            raise AssertionError(f"missing test input {p}")
    with tempfile.TemporaryDirectory(prefix="ores-checked-array-source-") as folder:
        temp = Path(folder)
        shim = temp / "test_wrappers.c"
        shim.write_text(C_WRAPPERS, encoding="utf-8")
        for label, src, owned, growing, heap in (
            ("lexical", LEXICAL, 2, 2, 0),
            ("class", CLASS, 4, 4, 0),
            ("moved-class", MOVED_CLASS, 2, 2, 0),
            ("replaced-field", REPLACED_FIELD, 3, 2, 0),
            ("mixed", MIXED, 1, 1, 1),
        ):
            source = temp / f"{label}.ores"
            source.write_text(src, encoding="utf-8")
            flags = ["--experimental-owned-int"] if heap else []
            compiled = run([str(compiler), *flags, str(source)])
            if compiled.returncode or compiled.stderr or not compiled.stdout:
                raise AssertionError(f"{label}: compiler failure {compiled.stderr!r}")
            ir = compiled.stdout
            required = [
                b"@oreslang_int_array_try_region_open",
                b"@oreslang_int_array_try_new",
                b"@oreslang_int_array_try_add",
                b"array.resource.failure:",
                b"array.failure.drop",
                b"array.failure.status.slot",
                b"array.failure.typed.status",
                b"array.failure.limit:",
                b"array.failure.no-memory:",
                b"array.failure.invalid-status:",
                b"switch i32",
                b"@oreslang_int_array_drop",
                b"@oreslang_int_array_region_close",
                b"@abort",
            ]
            for item in required:
                if item not in ir:
                    raise AssertionError(f"{label}: IR missing {item!r}")
            # Every checked new/add edge must carry the *actual* runtime
            # status into the distinct failure classifier before cleanup.
            # Nonrecoverable paths still abort after exact-once destruction.
            if (len(re.findall(rb'^\w[\w.]*\.array\.create\.failure(?:[0-9]+)?:', ir,
                               re.MULTILINE)) != owned or
                    len(re.findall(rb'^array\.add\.error(?:[0-9]+)?:', ir,
                                   re.MULTILINE)) != growing):
                raise AssertionError(f"{label}: lost status-preserving create/add failure edges")
            if ir.count(b"switch i32") != 1:
                raise AssertionError(f"{label}: ambiguous resource error classifier")
            if (ir.count(b"array.failure.limit:") != 1 or
                    ir.count(b"array.failure.no-memory:") != 1 or
                    ir.count(b"array.failure.invalid-status:") != 1):
                raise AssertionError(f"{label}: missing distinct resource statuses")
            if (ir.count(b"call i32 @oreslang_int_array_try_new(") != owned or
                    ir.count(b"call i32 @oreslang_int_array_try_add(") != growing or
                    b"call i64 @oreslang_int_array_new(" in ir or
                    b"call void @oreslang_int_array_add(" in ir):
                raise AssertionError(f"{label}: wrong native allocation/growth calls")
            ll = temp / f"{label}.ll"
            ll.write_bytes(ir)
            exe = temp / label
            link = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                    "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1"]
            if sanitizers:
                link += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            link += [
                "-Wl,--wrap=oreslang_int_array_drop",
                "-Wl,--wrap=oreslang_int_array_region_close",
                "-Wl,--wrap=abort",
                "-Wl,--wrap=oreslang_owned_i64_region_drop",
                "-Wl,--wrap=oreslang_owned_i64_region_close",
                "-x", "ir", str(ll),
                "-x", "c", str(runtime),
                str(root / "src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"),
                str(shim),
                "-I", str(root / "src/main/cpp"),
                "-o", str(exe),
            ]
            linked = run(link)
            if linked.returncode:
                raise AssertionError(f"{label}: clang/link: {linked.stderr!r}")
            # Allocation attempt 0 is try_region_open; attempts 1..owned
            # create owner handles, and subsequent attempts grow the arrays.
            for failed_at in range(1 + owned + growing):
                expected_drops = min(max(failed_at - 1, 0), owned)
                e = os.environ.copy()
                e.update(ORE_FAIL_AFTER=str(failed_at),
                         ORE_EXPECT_DROPS=str(expected_drops),
                         ORE_EXPECT_CLOSES=str(int(failed_at > 0)),
                         ORE_EXPECT_OWNED_DROPS=str(heap if failed_at > 0 else 0),
                         ORE_EXPECT_OWNED_CLOSES=str(heap))
                result = run([str(exe)], env=e)
                if result.returncode != 86 or \
                        b"CHECKED_ABORT_OK\n" not in result.stderr:
                    raise AssertionError(
                        f"{label} fault {failed_at}: expected exact cleanup; "
                        f"return={result.returncode}, stderr={result.stderr[:2200]!r}")
            # When the fault injector is past the final allocation, the
            # program must return 42 and the destructor must see each owner
            # destroyed exactly once plus one region close.
            e = os.environ.copy()
            e.update(ORE_FAIL_AFTER=str(1 + owned + growing),
                     ORE_EXPECT_DROPS=str(owned), ORE_EXPECT_CLOSES="1",
                     ORE_EXPECT_OWNED_DROPS=str(heap),
                     ORE_EXPECT_OWNED_CLOSES=str(heap))
            result = run([str(exe)], env=e)
            if result.returncode != 42 or result.stdout or result.stderr:
                raise AssertionError(
                    f"{label}: normal cleanup failed: rc={result.returncode}, "
                    f"stderr={result.stderr[:2200]!r}")
        # The new compiler-only four-field transaction has its own staging
        # allocations (4 handle creations + 4 backing buffers). The runtime
        # rolls back partial preparation internally without publishing any
        # staged handle. Every failed edge must still close the region with
        # no live owners, and a successful commit must leave one owner of
        # each filled backing array.
        source = temp / "four-batch.ores"
        source.write_text(BATCH_COMMIT, encoding="utf-8")
        compiled = run([str(compiler), str(source)])
        if compiled.returncode or compiled.stderr or not compiled.stdout:
            raise AssertionError(f"four-batch compiler failure: {compiled.stderr!r}")
        ir = compiled.stdout
        for symbol in (b"@oreslang_int_array_try_prepare_filled_many",
                       b"@oreslang_int_array_commit_many",
                       b"arrays.commit.prepare.failure:",
                       b"array.failure.status.slot"):
            if symbol not in ir:
                raise AssertionError(f"four-batch missing compiler IR {symbol!r}")
        if ir.count(b"call void @oreslang_int_array_commit_many(") != 1:
            raise AssertionError("four-batch must emit exactly one native commit")
        ll = temp / "four-batch.ll"
        ll.write_bytes(ir)
        exe = temp / "four-batch"
        link = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1"]
        if sanitizers:
            link += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        link += [
            "-Wl,--wrap=oreslang_int_array_drop",
            "-Wl,--wrap=oreslang_int_array_region_close",
            "-Wl,--wrap=abort",
            "-Wl,--wrap=oreslang_owned_i64_region_drop",
            "-Wl,--wrap=oreslang_owned_i64_region_close",
            "-x", "ir", str(ll), "-x", "c", str(runtime),
            str(root / "src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"),
            str(shim), "-I", str(root / "src/main/cpp"), "-o", str(exe),
        ]
        linked = run(link)
        if linked.returncode:
            raise AssertionError(f"four-batch clang/link: {linked.stderr!r}")
        # 0 region open; 1..4 four original class handles; 5..12 four
        # staging handles/buffers. Failed staging must preserve old owners.
        for failed_at in range(13):
            expected_drops = min(max(failed_at - 1, 0), 4)
            env = os.environ.copy()
            env.update(ORE_FAIL_AFTER=str(failed_at),
                       ORE_EXPECT_DROPS=str(expected_drops),
                       ORE_EXPECT_CLOSES=str(int(failed_at > 0)),
                       ORE_EXPECT_OWNED_DROPS="0",
                       ORE_EXPECT_OWNED_CLOSES="0")
            result = run([str(exe)], env=env)
            if result.returncode != 86 or b"CHECKED_ABORT_OK\n" not in result.stderr:
                raise AssertionError(
                    f"four-batch resource edge {failed_at}: "
                    f"rc={result.returncode}; stderr={result.stderr[:2200]!r}")
        env = os.environ.copy()
        env.update(ORE_FAIL_AFTER="13", ORE_EXPECT_DROPS="4",
                   ORE_EXPECT_CLOSES="1", ORE_EXPECT_OWNED_DROPS="0",
                   ORE_EXPECT_OWNED_CLOSES="0")
        result = run([str(exe)], env=env)
        if result.returncode != 42 or result.stdout or result.stderr:
            raise AssertionError(
                f"four-batch success cleanup: rc={result.returncode}, "
                f"stderr={result.stderr[:2200]!r}")
    print("PASS checked Array<int> source failure cleanup: "
          "41 injected allocation edges + 6 successful complete guest runs"
          + (" (ASan+UBSan)" if sanitizers else ""), flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    p.add_argument("--source-root", type=Path, required=True)
    p.add_argument("--sanitizers", action="store_true")
    options = p.parse_args()
    try:
        verify(options.compiler.resolve(), options.clang.resolve(),
               options.source_root.resolve(), options.sanitizers)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"checked Array<int> source failure gate FAILED: {error}", file=sys.stderr)
        sys.exit(1)
