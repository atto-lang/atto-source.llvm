#!/usr/bin/env python3
"""Exact native LLVM AOT counter: parser -> IR -> Clang -> actor-owned C ABI.

A compiler-generated on_message runs under real actor heap turn tokens.
The *separate* declared-actor + attached-state syntax remains fail-closed.
"""
import argparse
import pathlib
import subprocess
import tempfile

if not __debug__:
    raise SystemExit("native counter execution forbids Python -O")

def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=90)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--sanitize", action="store_true")
    opts = parser.parse_args()
    root = pathlib.Path(opts.root)
    fixture = root / "tests" / "fixtures" / "source-actor-counter-four.ores"
    unsupported = root / "tests" / "fixtures" / "declare-state-counter-native-unsupported.atto"
    harness = root / "tests" / "source_actor_counter_four_harness.c"
    native = root / "src" / "main" / "cpp" / "oreslang" / "runtime"
    include = root / "src" / "main" / "cpp"

    # An unsupported declared/attached actor must never emit runnable IR.
    rejected = run([opts.compiler, str(unsupported)])
    assert rejected.returncode != 0, "unverified declared actor source compiled"
    assert not rejected.stdout.strip(), "unverified declared actor emitted LLVM IR"

    compiled = run([opts.compiler, str(fixture)])
    assert compiled.returncode == 0, (compiled.returncode, compiled.stderr)
    assert compiled.stdout.startswith("; ModuleID"), "not LLVM IR"
    for symbol in ("oreslang_actor_Counter_init",
                   "oreslang_actor_Counter_on_message",
                   "ores_actor_i64_heap_field_write"):
        assert symbol in compiled.stdout, ("missing executable actor field codegen", symbol)

    with tempfile.TemporaryDirectory(prefix="atto-native-counter-four-") as folder:
        work = pathlib.Path(folder)
        ll = work / "counter.ll"
        exe = work / "counter"
        ll.write_text(compiled.stdout)
        flags = (["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
                 if opts.sanitize else ["-O1"])
        cmd = [opts.clang, *flags,
               "-x", "ir", str(ll),
               "-x", "c", str(harness), str(native / "ActorOwnedI64Heap.c"),
               "-std=c11", "-pthread", "-I", str(include), "-o", str(exe)]
        linked = run(cmd)
        assert linked.returncode == 0, (cmd, linked.stderr)
        executed = run([str(exe)])
        assert executed.returncode == 0, (executed.returncode, executed.stderr)
        assert executed.stderr.count("LLVM_COUNTER_FOUR_VERIFIED left=10 right=15 turns=8") == 1, (
            "native compiled Counter did not execute eight correct checked turns",
            executed.stderr)
    print("PASS real LLVM guest Counter: left=10 right=15, independent actor heaps, 8 turns")

if __name__ == "__main__":
    main()
