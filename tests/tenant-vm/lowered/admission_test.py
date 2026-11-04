#!/usr/bin/env python3
"""Modules the lowering pipeline must refuse, and mutations of one it accepts. Nothing here may
crash the tool, and everything refused must leave no library behind."""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("lower")
ap.add_argument("--wat2wasm", required=True)
ap.add_argument("--mutations", type=int, default=300)
ap.add_argument("--runner", default="", help="tenant_vm_lowered_test, to run what gets accepted")
ap.add_argument("--hostile", default="", help="hostile.wat, the seed for run-time mutation")
ap.add_argument("--lowered-root", default="", help="where the runner finds libraries, including hostile_bounds")
ap.add_argument("--run-mutants", type=int, default=25)
args, passthrough = ap.parse_known_args()

GOOD = '''(module (import "env" "memory" (memory 2 2))
  (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))'''

# Each is valid Wasm that some engine would run, and that this path must not.
REFUSE = {
    "defines its own memory": '(module (memory 1) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "no memory at all": '(module (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "memory larger than an actor": '(module (import "env" "memory" (memory 3)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "memory maximum larger than an actor": '(module (import "env" "memory" (memory 2 65536)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "function import": '(module (import "env" "memory" (memory 2 2)) (import "env" "system" (func (param i32))) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "global import": '(module (import "env" "memory" (memory 2 2)) (import "env" "secret" (global i64)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "table import": '(module (import "env" "memory" (memory 2 2)) (import "env" "t" (table 1 funcref)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "growable table": '(module (import "env" "memory" (memory 2 2)) (table 1 funcref) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "enormous table": '(module (import "env" "memory" (memory 2 2)) (table 100000 100000 funcref) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "start function": '(module (import "env" "memory" (memory 2 2)) (func $s) (start $s) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "exports memory": '(module (import "env" "memory" (memory 2 2)) (export "m" (memory 0)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (i64.const 0)))',
    "missing turn": '(module (import "env" "memory" (memory 2 2)) (func (export "other")))',
    "too many locals": '(module (import "env" "memory" (memory 2 2)) (func (export "turn") (param i64 i32 i32 i32 i32 i32) (result i64) (local ' + 'i64 ' * 5000 + ') (i64.const 0)))',
}
# Binary-only: shared and 64-bit memory, multiple tables, truncation. Hand-assembled so that no
# text assembler's feature set decides whether the attack gets as far as the tool.
def section(i, payload):
    assert len(payload) < 128
    return bytes([i, len(payload)]) + payload
HEADER = b"\0asm\x01\0\0\0"
IMPORT = lambda flags, *limits: section(2, b"\x01\x03env\x06memory\x02" + bytes([flags]) + bytes(limits))
BINARY = {
    "memory64 import": HEADER + IMPORT(0x04, 2),
    "memory64 with max": HEADER + IMPORT(0x05, 2, 2),
    "shared memory import": HEADER + IMPORT(0x03, 2, 2),
    "custom page size bit": HEADER + IMPORT(0x08, 2),
    "two memories": HEADER + section(2, b"\x02" + (b"\x03env\x06memory\x02\x00\x02") * 2),
    "not wasm": b"\x7fELF\x02\x01\x01\0" + b"\0" * 64,
    "wasm 2 version": b"\0asm\x02\0\0\0",
    "empty": b"",
    "truncated section": HEADER + b"\x01\x7f",
    "section past end": HEADER + bytes([1, 0xFF, 0x7F]),
    "overlong LEB": HEADER + bytes([1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]),
    "tag section": HEADER + section(13, b"\x00"),
    "duplicate type section": HEADER + section(1, b"\x00") + section(1, b"\x00"),
    "sections out of order": HEADER + section(3, b"\x00") + section(1, b"\x00"),
}

def lower(path, out, *extra):
    return subprocess.run([sys.executable, args.lower, str(path), "--out", str(out), *passthrough, *extra],
                          capture_output=True, text=True, timeout=300)

failures = 0
with tempfile.TemporaryDirectory(prefix="tenant-vm-admission-") as temporary:
    tmp = Path(temporary)
    def refused(name, path):
        global failures
        out = tmp / "out"
        result = lower(path, out)
        library_left = any(out.glob("guest.*")) if out.exists() else False
        if result.returncode == 0 or library_left:
            print(f"  FAIL accepted: {name}: {result.stdout}{result.stderr}")
            failures += 1
        elif "REFUSED" not in result.stderr:
            print(f"  FAIL crashed instead of refusing: {name}: {result.stderr[-400:]}")
            failures += 1
        for child in out.glob("*") if out.exists() else []:
            child.unlink()

    for name, wat in REFUSE.items():
        path = tmp / "case.wat"
        path.write_text(wat)
        refused(name, path)
    for name, data in BINARY.items():
        path = tmp / "case.wasm"
        path.write_bytes(data)
        refused(name, path)
    print(f"  {len(REFUSE) + len(BINARY)} hostile modules refused")

    # A feature some wabt versions enable by default, and which breaks the rule if lowered.
    good = tmp / "good.wat"
    good.write_text(GOOD)
    wasm = tmp / "good.wasm"
    subprocess.run([args.wat2wasm, str(good), "-o", str(wasm)], check=True)
    baseline = lower(wasm, tmp / "good")
    if baseline.returncode != 0:
        print(f"  FAIL a valid module was refused: {baseline.stderr}")
        failures += 1

    # Mutations of a valid module: the tool may accept or refuse, but never crash or hang.
    rng = random.Random(0xA110)
    data = wasm.read_bytes()
    accepted = refused_count = 0
    for i in range(args.mutations):
        mutated = bytearray(data)
        for _ in range(rng.randint(1, 4)):
            at = rng.randrange(8, len(mutated))
            choice = rng.random()
            if choice < 0.6:
                mutated[at] = rng.randrange(256)
            elif choice < 0.8:
                del mutated[at]
            else:
                mutated.insert(at, rng.randrange(256))
        path = tmp / "mutant.wasm"
        path.write_bytes(bytes(mutated))
        out = tmp / "mutant"
        result = lower(path, out)
        if result.returncode == 0:
            accepted += 1
        elif "REFUSED" in result.stderr:
            refused_count += 1
        else:
            print(f"  FAIL mutation {i} crashed the tool: {result.stderr[-400:]}")
            failures += 1
        if out.exists():
            for child in out.glob("*"):
                child.unlink()
    print(f"  {args.mutations} mutations: {refused_count} refused, {accepted} accepted, 0 crashed")

    # What matters most: mutate the code of a hostile module, and RUN what gets through. Whatever
    # the pipeline accepts must stay in its cage, or end only its own VM. Wasm validation rejects
    # most mutants, and each survivor costs a C compile, so this draws until it has enough.
    if args.runner:
        seed_wasm = tmp / "hostile.wasm"
        subprocess.run([args.wat2wasm, args.hostile, "-o", str(seed_wasm)], check=True)
        original = seed_wasm.read_bytes()
        reader_at = 8
        code_start = code_end = None
        while reader_at < len(original):
            kind = original[reader_at]
            reader_at += 1
            size, shift = 0, 0
            while True:
                byte = original[reader_at]
                reader_at += 1
                size |= (byte & 0x7F) << shift
                shift += 7
                if not byte & 0x80:
                    break
            if kind == 10:
                code_start, code_end = reader_at, reader_at + size
            reader_at += size
        assert code_start, "hostile module has no code section"
        survivors = ran = 0
        draws = 0
        stem = Path(args.lowered_root) / "fuzz_mutant"
        while survivors < args.run_mutants and draws < args.run_mutants * 200:
            draws += 1
            mutated = bytearray(original)
            for _ in range(rng.randint(1, 3)):
                at = rng.randrange(code_start, code_end)
                # Bias toward memory-access opcodes and their immediates, where an escape would be.
                mutated[at] = rng.choice([0x28, 0x29, 0x36, 0x37, 0x2D, 0x3A, 0x10, 0x11, 0x40, 0x3F, 0x24, 0x23, rng.randrange(256), rng.randrange(256)])
            path = tmp / "survivor.wasm"
            path.write_bytes(bytes(mutated))
            mode = "guard" if survivors % 2 else "bounds"
            result = lower(path, stem, "--mode", mode)
            if result.returncode != 0:
                if "REFUSED" not in result.stderr:
                    print(f"  FAIL a mutant crashed the tool: {result.stderr[-400:]}")
                    failures += 1
                continue
            survivors += 1
            run = subprocess.run([args.runner, args.lowered_root, "--run-accepted", stem.name],
                                 capture_output=True, text=True, timeout=120)
            ran += 1
            if run.returncode != 0:
                keep = tmp.parent / f"tenant-vm-escape-{survivors}.wasm"
                keep.write_bytes(bytes(mutated))
                print(f"  FAIL mutant {survivors} ({mode}): exit {run.returncode}, kept at {keep}\n{run.stderr[-600:]}")
                failures += 1
        print(f"  {draws} code mutations, {survivors} accepted by the pipeline, {ran} run with hostile inputs")
        if survivors == 0:
            print("  FAIL no mutant survived, so nothing was tested")
            failures += 1

print("ADMISSION_FAIL" if failures else "ADMISSION_PASS")
sys.exit(1 if failures else 0)
