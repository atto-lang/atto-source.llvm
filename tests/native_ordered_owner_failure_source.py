#!/usr/bin/env python3
"""Compiler-backed ordered owner cross-frame failure cleanup gate.

An allocation failure inside a factory must retire all live handles and
regions, including caller-owned handles from earlier successful factories.
This is deliberately a strict regression gate, not an expected-failure test.
"""
import argparse
import os
from pathlib import Path
import tempfile

from native_checked_array_failure_source import C_WRAPPERS, run


def verify(compiler, clang, root, sanitizers):
    fixtures = root / "tests/fixtures/ordered-owner-v1"
    with tempfile.TemporaryDirectory(prefix="ordered-owner-failure-") as directory:
        temp = Path(directory)
        shim = temp / "wrappers.c"
        shim.write_text(C_WRAPPERS, encoding="utf-8")
        failures = []
        for name, reached_factories in (("normal", 2), ("early", 1)):
            compiled = run([str(compiler), str(fixtures / (name + ".atto"))])
            if compiled.returncode or compiled.stderr or not compiled.stdout:
                raise AssertionError(f"{name}: compile failed: {compiled.stderr!r}")
            ir = temp / (name + ".ll")
            ir.write_bytes(compiled.stdout)
            exe = temp / name
            link = [str(clang), "-O1", "-std=c11", "-Wno-override-module",
                    "-DORESLANG_NATIVE_TEST_ALLOC_FAILURE=1"]
            if sanitizers:
                link += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            for symbol in ("oreslang_int_array_try_add",
                           "oreslang_int_array_unhandled_resource_failure",
                           "oreslang_int_array_drop", "oreslang_int_array_region_close",
                           "oreslang_owned_i64_region_drop", "oreslang_owned_i64_region_close",
                           "abort"):
                link.append("-Wl,--wrap=" + symbol)
            runtime = root / "src/main/cpp/oreslang/runtime"
            link += ["-x", "ir", str(ir), "-x", "c",
                     str(runtime / "NativeIntArrayRegion.c"),
                     str(runtime / "NativeOwnedI64Region.c"), str(shim),
                     "-I", str(root / "src/main/cpp"), "-o", str(exe)]
            linked = run(link)
            if linked.returncode:
                raise AssertionError(f"{name}: link failed: {linked.stderr!r}")

            def execute(fail_after, drops, closes, status, limit=False):
                env = os.environ.copy()
                env.pop("ORE_FORCE_ARRAY_LIMIT", None)
                env.update(ORE_FAIL_AFTER=str(fail_after),
                           ORE_EXPECT_DROPS=str(drops),
                           ORE_EXPECT_CLOSES=str(closes),
                           ORE_EXPECT_OWNED_DROPS="0", ORE_EXPECT_OWNED_CLOSES="0",
                           ORE_EXPECT_RESOURCE_STATUS=str(status))
                if limit:
                    env["ORE_FORCE_ARRAY_LIMIT"] = "1"
                result = run([str(exe)], env=env)
                wanted = 86 if status else 42
                marker = b"CHECKED_ABORT_OK\n" if status else b""
                if result.returncode != wanted or (status and marker not in result.stderr) or (
                        not status and (result.stdout or result.stderr)):
                    failures.append(f"{name} fail_after={fail_after} limit={limit}: "
                                    f"expected drops={drops} closes={closes} status={status}; "
                                    f"actual rc={result.returncode} stderr={result.stderr[:800]!r}")
                else:
                    print(f"PASS {name} fail_after={fail_after} limit={limit} "
                          f"drops={drops} closes={closes} status={status}", flush=True)

            # Allocation 0 opens main's region. Each reached factory then
            # opens its own region, creates one handle, and grows its buffer.
            # Transfer is allocation-free: the token changes regions but
            # the number of owned handles is unchanged. Every successful
            # open and creation must have exactly one eventual close/drop,
            # including caller state when a later factory fails.
            execute(0, 0, 0, 2)
            for i in range(reached_factories):
                first = 1 + 3 * i
                execute(first, i, 1 + i, 2)
                execute(first + 1, i, 2 + i, 2)
                execute(first + 2, i + 1, 2 + i, 2)
            execute(-1, 1, 2, 1, limit=True)
            execute(1 + 3 * reached_factories, reached_factories,
                    1 + reached_factories, 0)
        if failures:
            raise AssertionError("\n".join(failures))
        print("ORDERED_OWNER_CROSS_FRAME_FAILURE_CLEANUP_PASS", flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    p.add_argument("--source-root", type=Path, required=True)
    p.add_argument("--sanitizers", action="store_true")
    a = p.parse_args()
    verify(a.compiler.resolve(), a.clang.resolve(), a.source_root.resolve(), a.sanitizers)
