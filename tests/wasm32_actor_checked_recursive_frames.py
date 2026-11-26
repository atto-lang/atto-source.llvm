#!/usr/bin/env python3
"""Real compiler-backed, opt-in bounded Wasm actor recursion (no JS interpreter).

The LLVM frontend parses/validates unchanged .atto and produces an iterative
Wasm actor-frame state machine. The guest's 65th managed push fails before
native engine stack exhaustion and exposes a distinct terminal status.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser()
for flag in ("compiler", "clang", "node", "fixture"):
    p.add_argument("--" + flag, required=True)
args = p.parse_args()
source = Path(args.fixture)
assert source.suffix == ".atto", "new guest fixtures must be canonical .atto"
atto = source.read_bytes()
blob = hashlib.sha1(f"blob {len(atto)}\0".encode() + atto).hexdigest()
assert blob == "947781cddf09d8e3e08c51cff2b7d8960e2d4750"
compiler = Path(args.compiler)
binary_hash = hashlib.sha256(compiler.read_bytes()).hexdigest()

def run(argv, stdin=None, permitted=False):
    r = subprocess.run(argv, input=stdin, text=True, capture_output=True,
                       timeout=75, check=False)
    if not permitted and r.returncode != 0:
        raise AssertionError((argv, r.returncode, r.stderr[:1200], r.stdout[:1200]))
    return r

flags = ["--target=wasm32", "--experimental-wasm-actor-recursion-v1"]
ir = run([str(compiler), *flags, str(source)]).stdout
assert 'target triple = "wasm32-unknown-unknown"' in ir
assert re.search(r"@oreslang\.wasm\.actor\.frame\.slots = internal global \[64 x i64\]", ir)
for value in ("atto.trampoline.push", "atto.trampoline.unwind",
              "atto.trampoline.overflow", "atto.frame.private.slot",
              "oreslang_wasm_actor_frame_status", "llvm.trap"):
    assert value in ir, "missing compiled guest-frame lowering: " + value
assert "getelementptr inbounds [64 x i64]" in ir
assert "store volatile i64" in ir
assert "load volatile i64" in ir
assert re.search(r"store volatile i32 1,", ir), (
    "optimizer may eliminate terminal status before trapping")
assert "load volatile i32" in ir, "typed terminal status export must read volatile"
assert "@descend(" not in ir, "recursive native engine call was emitted"
assert not re.search(r"\balloca\b", ir), "trampoline must not allocate transient native stack locals"
assert re.search(r"atto\.trampoline\.current = phi i64", ir)
assert re.search(r"atto\.trampoline\.answer = phi i64", ir)
assert "declare" not in "\n".join(
    line for line in ir.splitlines()
    if "descend" in line), "unlowered recursive helper reached wasm32 IR"
assert "oreslang_source_actor_spawn" not in ir
assert "ores_actor_i64_heap_field" not in ir
assert "@main(" not in ir, "specialized Wasm actor module may export no host main"
assert "@oreslang_wasm_actor_frame_status()" in ir

node = r"""
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
const bytes=readFileSync(process.argv[1]);
assert.equal(WebAssembly.validate(bytes), true);
const module=new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), []);
const ex=WebAssembly.Module.exports(module);
assert.equal(ex.length, 5);
for(const [name,kind] of Object.entries({
  oreslang_wasm_actor_init:"function",
  oreslang_wasm_actor_on_message:"function",
  oreslang_wasm_actor_teardown:"function",
  oreslang_wasm_actor_frame_status:"function",
  memory:"memory"
})) assert.ok(ex.some(x=>x.name===name && x.kind===kind),name);
const instantiate=()=>new WebAssembly.Instance(module,{}).exports;
const a=instantiate(), b=instantiate();
assert.notEqual(a.memory.buffer,b.memory.buffer,"actor frames must not alias");
assert.equal(a.memory.buffer.byteLength,131072);
assert.equal(a.oreslang_wasm_actor_frame_status(),0);
assert.throws(()=>a.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
a.oreslang_wasm_actor_init();
b.oreslang_wasm_actor_init();
for(const [depth,result] of [[0n,0n],[1n,1n],[2n,2n],[63n,63n]])
 assert.equal(a.oreslang_wasm_actor_on_message(depth),result);
assert.equal(a.oreslang_wasm_actor_frame_status(),0);
const negative=instantiate();negative.oreslang_wasm_actor_init();
assert.throws(()=>negative.oreslang_wasm_actor_on_message(-1n),WebAssembly.RuntimeError);
assert.equal(negative.oreslang_wasm_actor_frame_status(),0,
 "invalid negative recursion must not be mislabeled as stack overflow");
assert.equal(b.oreslang_wasm_actor_on_message(5n),5n);
assert.throws(()=>a.oreslang_wasm_actor_on_message(64n),WebAssembly.RuntimeError);
assert.equal(a.oreslang_wasm_actor_frame_status(),1,"typed terminal stack quota");
assert.throws(()=>a.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
assert.throws(()=>a.oreslang_wasm_actor_teardown(),WebAssembly.RuntimeError);
assert.equal(b.oreslang_wasm_actor_on_message(2n),2n,
 "B's private memory and Wasm stack are unaffected by A");
assert.equal(b.oreslang_wasm_actor_frame_status(),0);
const fresh=instantiate();
fresh.oreslang_wasm_actor_init();
assert.equal(fresh.oreslang_wasm_actor_on_message(3n),3n);
assert.equal(fresh.oreslang_wasm_actor_frame_status(),0);
fresh.oreslang_wasm_actor_teardown();
b.oreslang_wasm_actor_teardown();
assert.throws(()=>b.oreslang_wasm_actor_on_message(1n),WebAssembly.RuntimeError);
console.log("COMPILED_ATTO_ACTOR_65TH_FRAME_DENIED=YES");
console.log("COMPILED_ATTO_ACTOR_B_INDEPENDENT=YES");
"""

with tempfile.TemporaryDirectory(prefix="atto-checked-actor-stack-") as folder:
    root = Path(folder)
    outcomes = []
    for opt in ("-O0", "-Oz"):
        wasm = root / (opt[1:] + ".wasm")
        link = run([args.clang, "--target=wasm32", "-x", "ir", opt,
            "-nostdlib", "-Wl,--no-entry", "-Wl,--export-memory",
            "-Wl,--export=oreslang_wasm_actor_init",
            "-Wl,--export=oreslang_wasm_actor_on_message",
            "-Wl,--export=oreslang_wasm_actor_teardown",
            "-Wl,--export=oreslang_wasm_actor_frame_status",
            "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
            "-o", str(wasm), "-"], stdin=ir)
        assert not link.stdout.strip(), link.stdout
        observed = run([args.node, "--input-type=module", "-e", node, str(wasm)])
        assert observed.stdout.count("COMPILED_ATTO_ACTOR_65TH_FRAME_DENIED=YES") == 1
        assert observed.stdout.count("COMPILED_ATTO_ACTOR_B_INDEPENDENT=YES") == 1
        outcomes.append({
            "kind":"actual-source-wasm32-checked-actor-recursion-v1",
            "opt":opt, "fixture_blob":blob,
            "source_sha256":hashlib.sha256(atto).hexdigest(),
            "compiler_sha256":binary_hash,
            "ir_sha256":hashlib.sha256(ir.encode()).hexdigest(),
            "wasm_sha256":hashlib.sha256(wasm.read_bytes()).hexdigest(),
            "host_engine_stack_recursive_calls":0,
            "max_actor_guest_frames":64,
            "first_rejected_frame":65,
            "actor_a_status":"STACK_BUDGET_EXHAUSTED",
            "actor_b_result":2,
            "same_worker_actor_fault_isolation":"NOT_TESTED_BY_THIS_DIRECT_INSTANCE_GATE",
            "supervisor_restart":"NOT_TESTED"
        })
        print(json.dumps(outcomes[-1],sort_keys=True))

    # Explicit option is required, and conflicting targets/duplicates cannot
    # silently use old unchecked native recursive calls.
    for option in (
        ["--target=host", "--experimental-wasm-actor-recursion-v1"],
        ["--experimental-wasm-actor-recursion-v1"],
        [*flags, "--experimental-wasm-actor-recursion-v1"],
        ["--target=wasm32", "--tree-shake", "--experimental-wasm-actor-recursion-v1"]
    ):
        fail = run([str(compiler), *option, str(source)], permitted=True)
        assert fail.returncode != 0 and not fail.stdout.strip(), option

    # Wrong recursion grammar, extra runtime effect, or non-final parameter
    # must fail before any LLVM IR/guest artifact is published.
    original = atto.decode()
    mutations = [
        ("step-two", "descend(depth - 1)", "descend(depth - 2)"),
        ("named-recursive-argument", "descend(depth - 1)", "descend(depth: depth - 1)"),
        ("named-handler-argument", "return descend(depth);", "return descend(depth: depth);"),
        ("multiply-unwind", "1 + descend(depth - 1)", "2 + descend(depth - 1)"),
        ("extra-actor-effect", "return descend(depth);",
             "total = total + 1;\n    return descend(depth);"),
        ("wrong-actor-argument", "return descend(depth);", "return descend(5);"),
        ("nested-helper-nonfinal", "1 + descend(depth - 1)",
             "descend(depth - 1) + descend(depth - 1)"),
    ]
    for name, before, after in mutations:
        assert original.count(before) == 1, name
        negative = root / (name + ".atto")
        negative.write_text(original.replace(before, after))
        invalid = run([str(compiler), *flags, str(negative)], permitted=True)
        assert invalid.returncode != 0, name
        assert not invalid.stdout.strip(), name
        assert "oreslang-llvmc:" in invalid.stderr, (name, invalid.stderr)
        assert not (root / (name + ".wasm")).exists()
print("PASS restricted compiler-managed recursive actor frames and fail-closed negatives (O0/Oz)")
