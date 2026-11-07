#!/usr/bin/env python3
"""Actual AttoLang compiler -> LLVM IR -> wasm32 guest actor frame conformance.

The source AST is compiled with oreslang-llvmc. Never replace this with synthetic
LLVM IR or a JS reference actor. The frame backend is still a scalar subset.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
for key in ("compiler", "clang", "node", "fixtures"):
    parser.add_argument("--" + key, required=True)
args = parser.parse_args()
fixture_dir = Path(args.fixtures)
source = fixture_dir / "wasm32-single-source-actor.atto"
legacy = fixture_dir / "wasm32-single-source-actor.ores"
atto = source.read_bytes()
assert atto == legacy.read_bytes(), "canonical .atto source drift from compiler legacy fixture"
blob = hashlib.sha1(f"blob {len(atto)}\0".encode() + atto).hexdigest()
assert blob == "adfc0b43463d7ce92d32a4b1b63602f91e527329", blob
source_hash = hashlib.sha256(atto).hexdigest()
compiler_hash = hashlib.sha256(Path(args.compiler).read_bytes()).hexdigest()

def run(argv, *, stdin=None):
    result = subprocess.run(
        argv, input=stdin, text=True, capture_output=True, timeout=75)
    if result.returncode != 0 or result.stderr and "warning:" not in result.stderr:
        raise AssertionError((argv, result.returncode, result.stdout[:1000],
                              result.stderr[:1000]))
    return result.stdout

ir = run([args.compiler, "--target=wasm32", str(source)])
assert 'target triple = "wasm32-unknown-unknown"' in ir
assert "@oreslang.wasm.actor.frame.depth" in ir
assert re.search(r"@oreslang\.wasm\.actor\.frame\.slots = internal global \[64 x i64\]", ir)
assert "atto.actor.guest.frame.depth.before" in ir
assert "atto.actor.guest.frame.message" in ir
assert "atto.actor.guest.frame.message.read" in ir
assert "atto.actor.guest.frame.denied" in ir
assert "atto.actor.guest.frames.clean" in ir
assert "atto.actor.guest.frames.live" in ir
assert "getelementptr inbounds [64 x i64]" in ir
assert len(re.findall(r"store volatile i64", ir)) >= 3
assert "load volatile i64" in ir
assert not re.search(r"message\.slot\s*=\s*alloca\s+i64", ir)
for symbol in ("oreslang_wasm_actor_init", "oreslang_wasm_actor_on_message",
               "oreslang_wasm_actor_teardown"):
    assert "@" + symbol + "(" in ir
assert "oreslang_source_actor_spawn" not in ir
assert "ores_actor_i64_heap_field" not in ir
assert "declare void @oreslang_actor_guest_frame" not in ir

node_case = r"""
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
const bytes = readFileSync(process.argv[1]);
assert.equal(WebAssembly.validate(bytes), true);
const module = new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), []);
const exports = WebAssembly.Module.exports(module);
assert.equal(exports.length, 4);
for (const entry of ["oreslang_wasm_actor_init",
                     "oreslang_wasm_actor_on_message",
                     "oreslang_wasm_actor_teardown", "memory"])
  assert.ok(exports.some(x => x.name === entry));
const actor = () => new WebAssembly.Instance(module, {}).exports;
const a = actor(), b = actor();
assert.notEqual(a.memory.buffer, b.memory.buffer);
assert.equal(a.memory.buffer.byteLength, 131072);
assert.equal(b.memory.buffer.byteLength, 131072);
assert.throws(() => a.oreslang_wasm_actor_on_message(1n), WebAssembly.RuntimeError);
a.oreslang_wasm_actor_init();
b.oreslang_wasm_actor_init();
for (let i = 1; i <= 100; i++)
  assert.equal(a.oreslang_wasm_actor_on_message(1n), BigInt(i));
assert.equal(b.oreslang_wasm_actor_on_message(7n), 7n);
assert.equal(a.oreslang_wasm_actor_on_message(-10n), 90n);
assert.equal(b.oreslang_wasm_actor_on_message(2n), 9n);
a.oreslang_wasm_actor_teardown();
assert.throws(() => a.oreslang_wasm_actor_on_message(1n), WebAssembly.RuntimeError);
assert.throws(() => a.oreslang_wasm_actor_init(), WebAssembly.RuntimeError);
assert.equal(b.oreslang_wasm_actor_on_message(1n), 10n);
const fresh = actor();
fresh.oreslang_wasm_actor_init();
assert.equal(fresh.oreslang_wasm_actor_on_message(5n), 5n);
fresh.oreslang_wasm_actor_teardown();
b.oreslang_wasm_actor_teardown();
console.log("ATTO_WASM_ACTOR_GUEST_FRAME_SCALAR_EXECUTED=YES");
"""
with tempfile.TemporaryDirectory(prefix="atto-wasm-actor-guest-frame-") as folder:
    for opt in ("-O0", "-Oz"):
        binary = Path(folder) / ("actor-" + opt[1:] + ".wasm")
        run([args.clang, "--target=wasm32", "-x", "ir", opt, "-nostdlib",
             "-Wl,--no-entry", "-Wl,--export-memory",
             "-Wl,--export=oreslang_wasm_actor_init",
             "-Wl,--export=oreslang_wasm_actor_on_message",
             "-Wl,--export=oreslang_wasm_actor_teardown",
             "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
             "-o", str(binary), "-"], stdin=ir)
        output = run([args.node, "--input-type=module", "-e", node_case,
                      str(binary)])
        assert "ATTO_WASM_ACTOR_GUEST_FRAME_SCALAR_EXECUTED=YES" in output
        print(json.dumps({
            "kind": "actual-source-wasm-actor-frame",
            "opt": opt,
            "fixture_blob": blob,
            "fixture_sha256": source_hash,
            "compiler_sha256": compiler_hash,
            "wasm_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "test": "ATTO_WASM_ACTOR_GUEST_FRAME_SCALAR_EXECUTED=YES",
            "actor_frames": "one scalar parameter; bounded one active call",
            "stack_overflow": "UNPROVEN"
        }, sort_keys=True))
print("PASS compiler-owned Wasm scalar actor frame IR and guest instance isolation (O0/Oz)")
