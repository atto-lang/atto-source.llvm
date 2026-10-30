#!/usr/bin/env python3
"""Compile native sealed-state Oreslang actors into LLVM IR and execute them.

This checks actual guest on_message code with actor-owned heap epoch/turn
admission. Separate scheduler E2E validates the SourceActorBootstrap path.
Neither test claims suspended handle_state or multi-backend parity.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile


def checked(cmd, **kwargs):
    completed = subprocess.run(
        cmd, capture_output=True, text=True, timeout=120, check=False, **kwargs)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed exit={completed.returncode}: {cmd!r}\n"
            f"stdout={completed.stdout[-2500:]}\n"
            f"stderr={completed.stderr[-3500:]}")
    return completed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="oreslang-sealed-state-guest-") as tmp:
        directory = pathlib.Path(tmp)
        for kind in ("write", "read"):
            source = root / "tests/fixtures" / (
                f"source-isolated-actor-sealed-{kind}-actor-only.ores")
            ir = checked([args.compiler, str(source)]).stdout
            if not ir.startswith("; ModuleID"):
                raise RuntimeError(f"missing real LLVM IR for {kind}")
            for symbol in ("oreslang_actor_Counter_init",
                           "oreslang_actor_Counter_on_message"):
                if not re.search(r"(?m)^define\\s+.*@"
                                 + symbol + r"\\s*\\(", ir):
                    raise RuntimeError(f"missing compiler-defined {symbol}")
            if "gc.statepoint" in ir or "gc.relocate" in ir:
                raise RuntimeError("tracing GC inserted into native actor IR")
            ll = directory / f"{kind}.ll"
            ll.write_text(ir, encoding="utf-8")
            executable = directory / f"actor-{kind}"
            flags = (["-O1", "-g", "-fsanitize=address,undefined",
                      "-fno-omit-frame-pointer"]
                     if args.sanitize else ["-O2"])
            checked([
                args.clang, "-std=c11", "-pthread", "-UNDEBUG", "-Wall",
                "-Wextra", "-Wpedantic", "-Werror", "-Wno-error=override-module",
                *flags, "-I", str(root / "src/main/cpp"),
                "-x", "ir", str(ll), "-x", "c",
                str(root / "tests/native_isolated_sealed_state_guest_bridge_test.c"),
                str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
                "-o", str(executable),
            ])
            env = os.environ.copy()
            if args.sanitize:
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            for index in range(3 if args.sanitize else 12):
                result = checked([str(executable), kind], env=env)
                receipt = f"COMPILED_SEALED_{kind.upper()}_GUEST_PASS"
                if receipt not in result.stdout:
                    raise RuntimeError(f"run {index}: missing {kind} guest receipt")
    print("COMPILED_SEALED_STATE_GUEST_PASS "
          f"mode={'ASAN_UBSAN' if args.sanitize else 'RELEASE'}")


if __name__ == "__main__":
    main()
