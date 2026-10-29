#!/usr/bin/env python3
"""Compile real Oreslang guest segments and execute under native scheduler DATA/WAKE.

This intermediate test does NOT claim that 'suspends' actor method/await lowering
exists. The host bridge owns A's retained ticket, resume notification and state.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile

if not __debug__:
    raise SystemExit("compiled guest bridge must run with Python assertions enabled")


def checked(argv, **kwargs):
    result = subprocess.run(argv, capture_output=True, text=True,
                            timeout=90, check=False, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(
            f"failed ({result.returncode}): {argv!r}\n"
            f"stdout:\n{result.stdout[-2500:]}\n"
            f"stderr:\n{result.stderr[-4000:]}"
        )
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    root = args.root.resolve()
    guest_source = root / "tests/fixtures/native-retained-guest-steps.ores"
    harness = root / "tests/native_actor_compiled_guest_ledger_bridge_test.c"
    native = root / "src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
    rejected = root / "tests/fixtures/source-actor-suspend-method-reject.ores"

    # The old compiler must not turn unsupported guest suspension into a
    # successful host-only simulation. When new lowering lands, replace this
    # fail-closed check with a separately compiled suspension acceptance test.
    negative = subprocess.run([args.compiler, str(rejected)], capture_output=True,
                              text=True, timeout=60, check=False)
    diagnostic = "native suspends actor method requires continuation lowering"
    if (negative.returncode == 0 or diagnostic not in negative.stderr
            or negative.stdout.strip()):
        raise RuntimeError(
            "compiler suspension boundary changed; demand real guest-await "
            f"lowering tests, got exit={negative.returncode} "
            f"stdout={negative.stdout[-500:]} stderr={negative.stderr[-1200:]}"
        )
    print("FULL_SOURCE_ACTOR_SUSPENDS_LOWERING_STILL_REJECTED", flush=True)

    ir = checked([args.compiler, str(guest_source)]).stdout
    for symbol in ("guest_step", "guest_resume"):
        if not re.search(r"(?m)^define\s+i64\s+@" + symbol + r"\s*\(", ir):
            raise RuntimeError(f"missing actual guest LLVM function: {symbol}")
    if not re.search(r"(?m)^define\s+i64\s+@main\s*\(", ir):
        raise RuntimeError("guest module has no public compiled main")
    if "gc.statepoint" in ir or "gc.relocate" in ir:
        raise RuntimeError("tracing GC unexpectedly inserted in native guest IR")
    # A native C harness supplies its own main(). Rename only the checked
    # guest entry, including its symbol call references if any.
    renamed, count = re.subn(r"@main(?=\s*\()", "@ores_guest_fixture_main", ir)
    if count != 1:
        raise RuntimeError(f"guest main() rename expected once, got {count}")

    with tempfile.TemporaryDirectory(prefix="oreslang-retained-guest-") as d:
        directory = pathlib.Path(d)
        ll = directory / "guest.ll"
        ll.write_text(renamed, encoding="utf-8")
        binary = directory / "compiled-guest-actor"
        options = (["-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"])
        # Compile and link the LLVM IR AND scheduler host against one binary;
        # no simulation of the guest function or pre-generated C substitute.
        checked([
            args.clang, "-std=c11", "-pthread", "-UNDEBUG",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-Wno-error=override-module", *options,
            "-I", str(root / "src/main/cpp"),
            "-x", "ir", str(ll), "-x", "c", str(harness), str(native),
            "-o", str(binary),
        ])
        env = os.environ.copy()
        if args.sanitize:
            env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        runs = 3 if args.sanitize else 12
        for i in range(runs):
            result = checked([str(binary)], env=env)
            if "LLVM_COMPILED_ORES_SEGMENTS_RETAINED_LEDGER_PASS" not in result.stdout:
                raise RuntimeError(f"run {i}: missing actual guest execution receipt")
    mode = "ASAN_UBSAN" if args.sanitize else "RELEASE"
    print(f"LLVM_COMPILED_GUEST_RETAINED_SCHEDULER_PASS mode={mode} runs={runs}")


if __name__ == "__main__":
    main()
