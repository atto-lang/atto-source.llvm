#!/usr/bin/env python3
"""Executable, actual-source LLVM -> wasm32 guest actor proof (not an IR stand-in)."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

ap = argparse.ArgumentParser()
for flag in ("compiler", "clang", "node", "fixtures"):
    ap.add_argument("--" + flag, required=True)
args = ap.parse_args()

def invoke(argv, **kwargs):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=70, **kwargs)
    if r.returncode != 0:
        raise AssertionError(f"failed ({r.returncode}) {argv!r}\n{r.stderr}\n{r.stdout}")
    return r

fixtures = Path(args.fixtures)
source = fixtures / "wasm32-single-source-actor.ores"
ir = invoke([args.compiler, "--target=wasm32", str(source)]).stdout
assert 'target triple = "wasm32-unknown-unknown"' in ir
for symbol in ("oreslang_wasm_actor_init",
               "oreslang_wasm_actor_on_message",
               "oreslang_wasm_actor_teardown"):
    assert ("@" + symbol + "(") in ir, "missing source-generated export " + symbol
assert "oreslang_source_actor_spawn" not in ir
assert "ores_actor_i64_heap_field" not in ir

with tempfile.TemporaryDirectory(prefix="oreslang-real-source-wasm-actor-") as d:
    wasm = Path(d) / "guest.wasm"
    invoke([args.clang, "--target=wasm32", "-x", "ir", "-Oz", "-nostdlib",
            "-Wl,--no-entry",
            "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
            "-Wl,--export=main", "-Wl,--export-memory", "-Wl,--export=oreslang_wasm_actor_init",
            "-Wl,--export=oreslang_wasm_actor_on_message",
            "-Wl,--export=oreslang_wasm_actor_teardown",
            "-o", str(wasm), "-"], input=ir)
    # No synthetic IR, no native host ABI or imports. Invoke actual compiled
    # Oreslang actor handler against private Wasm globals.
    script = r"""
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
const bytes=readFileSync(process.argv[1]);
assert.ok(WebAssembly.validate(bytes));
const module=new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module),[]);
const mk=()=>new WebAssembly.Instance(module).exports;
const x=mk();
assert.equal(x.main(),42n);
assert.equal(x.memory.buffer.byteLength,131072);
assert.throws(()=>x.memory.grow(1),RangeError);
assert.throws(()=>x.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
x.oreslang_wasm_actor_init();
assert.throws(()=>x.oreslang_wasm_actor_init(),WebAssembly.RuntimeError);
for(let i=1;i<=100;i++)
 assert.equal(x.oreslang_wasm_actor_on_message(1n),BigInt(i));
const y=mk();
assert.notStrictEqual(x.memory.buffer,y.memory.buffer);
assert.equal(y.memory.buffer.byteLength,131072);
y.oreslang_wasm_actor_init();
assert.equal(y.oreslang_wasm_actor_on_message(7n),7n);
assert.equal(x.oreslang_wasm_actor_on_message(2n),102n);
x.oreslang_wasm_actor_teardown();
assert.throws(()=>x.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
assert.throws(()=>x.oreslang_wasm_actor_teardown(),WebAssembly.RuntimeError);
assert.throws(()=>x.oreslang_wasm_actor_init(),WebAssembly.RuntimeError);
assert.equal(y.oreslang_wasm_actor_on_message(1n),8n);
y.oreslang_wasm_actor_teardown();
console.log("ACTUAL_ORES_SOURCE_WASM_ACTOR_PASS 100 messages, private state, terminal lifecycle");
"""
    r = invoke([args.node, "--input-type=module", "-e", script, str(wasm)])
    assert "ACTUAL_ORES_SOURCE_WASM_ACTOR_PASS" in r.stdout

    # A second linker profile must match the current pooled host contract:
    # EXACTLY three lifecycle exports + private fixed-memory, no guest main.
    pooled = Path(d) / "actor-pooled.wasm"
    invoke([args.clang, "--target=wasm32", "-x", "ir", "-Oz", "-nostdlib",
            "-Wl,--no-entry", "-Wl,--export-memory",
            "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
            "-Wl,--export=oreslang_wasm_actor_init",
            "-Wl,--export=oreslang_wasm_actor_on_message",
            "-Wl,--export=oreslang_wasm_actor_teardown",
            "-o", str(pooled), "-"], input=ir)
    pool_script = r"""
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
const bytes=readFileSync(process.argv[1]);
assert.ok(WebAssembly.validate(bytes));
const mod=new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(mod),[]);
assert.deepEqual(WebAssembly.Module.exports(mod).map(e=>[e.name,e.kind]).sort(),
  [["memory","memory"],["oreslang_wasm_actor_init","function"],
   ["oreslang_wasm_actor_on_message","function"],
   ["oreslang_wasm_actor_teardown","function"]].sort());
const a=new WebAssembly.Instance(mod).exports;
const b=new WebAssembly.Instance(mod).exports;
assert.equal(a.memory.buffer.byteLength,131072);
assert.equal(b.memory.buffer.byteLength,131072);
assert.notStrictEqual(a.memory.buffer,b.memory.buffer);
assert.throws(()=>a.memory.grow(1),RangeError);
a.oreslang_wasm_actor_init();
b.oreslang_wasm_actor_init();
assert.equal(a.oreslang_wasm_actor_on_message(5n),5n);
assert.equal(b.oreslang_wasm_actor_on_message(7n),7n);
assert.equal(a.oreslang_wasm_actor_on_message(1n),6n);
a.oreslang_wasm_actor_teardown();
assert.throws(()=>a.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
assert.equal(b.oreslang_wasm_actor_on_message(1n),8n);
b.oreslang_wasm_actor_teardown();
console.log("REAL_SOURCE_POOLED_ACTOR_EXPORT_ABI_PASS");
"""
    pooled_receipt = invoke([args.node, "--input-type=module", "-e",
                             pool_script, str(pooled)])
    assert "REAL_SOURCE_POOLED_ACTOR_EXPORT_ABI_PASS" in pooled_receipt.stdout

# All unsupported native scheduler constructs must continue failing closed.
for case in ("source-actor-spawn-stop.ores", "source-actor-send.ores"):
    r = subprocess.run([args.compiler, "--target=wasm32", str(fixtures / case)],
                       capture_output=True, text=True, timeout=15)
    assert r.returncode != 0, case
    assert "wasm32 actor operations are not implemented" in r.stderr, r.stderr
    assert not r.stdout, case

print("PASS exact .ores -> compiler-generated wasm32 actor handler and lifecycle")
