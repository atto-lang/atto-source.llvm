#!/usr/bin/env python3
"""Compile a real Oreslang actor to wasm32 and exercise per-instance state."""
import argparse
import subprocess
import tempfile
from pathlib import Path

# Python -O removes every assert, including the real compiler and Wasm guest
# execution gates. Refuse optimization rather than emit a false-positive PASS.
if not __debug__:
    raise SystemExit("wasm32 source actor parity validation refuses optimized Python: assertions are required")

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
p.add_argument("--clang", required=True)
p.add_argument("--node", required=True)
p.add_argument("--fixture", required=True)
args = p.parse_args()

def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, text=True, timeout=45, **kwargs)

ir = run([args.compiler, "--target=wasm32", args.fixture])
assert ir.returncode == 0 and not ir.stderr, (ir.stderr, ir.stdout)
assert 'target triple = "wasm32-unknown-unknown"' in ir.stdout
assert '@oreslang_wasm_actor_init()' in ir.stdout
assert '@oreslang_wasm_actor_on_message(i64' in ir.stdout
assert "declare void @oreslang_source_actor_" not in ir.stdout

with tempfile.TemporaryDirectory(prefix="oreslang-actor-wasm32-") as folder:
    wasm = str(Path(folder) / "actor.wasm")
    link = run([
        args.clang, "--target=wasm32", "-x", "ir", "-Oz", "-nostdlib",
        "-Wl,--no-entry",
        "-Wl,--export=oreslang_wasm_actor_init",
        "-Wl,--export=oreslang_wasm_actor_on_message",
        "-Wl,--export=oreslang_wasm_actor_teardown",
        "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
        "-o", wasm, "-"
    ], input=ir.stdout)
    assert link.returncode == 0, link.stderr
    js = """
import {readFileSync} from 'node:fs';
const b=readFileSync(process.argv[1]);
if(!WebAssembly.validate(b))throw Error('invalid compiled Wasm');
const mod=new WebAssembly.Module(b);
if(WebAssembly.Module.imports(mod).length)throw Error('unexpected host import');
const a=new WebAssembly.Instance(mod),other=new WebAssembly.Instance(mod);
const init=a.exports.oreslang_wasm_actor_init;
const handle=a.exports.oreslang_wasm_actor_on_message;
if(typeof init!=='function'||typeof handle!=='function'||init.length!==0||handle.length!==1)
 throw Error('wrong generated actor ABI');
try{handle(1n);throw Error('no preinit trap')}catch(e){if(!(e instanceof WebAssembly.RuntimeError))throw e}
init();
if(handle(2n)!==12n||handle(3n)!==15n)throw Error('actor state lost across turns');
other.exports.oreslang_wasm_actor_init();
if(other.exports.oreslang_wasm_actor_on_message(1n)!==11n)
 throw Error('second module instance failed');
if(handle(1n)!==16n)throw Error('module-instance state isolation broken');
try{init();throw Error('duplicate init accepted')}catch(e){if(!(e instanceof WebAssembly.RuntimeError))throw e}
a.exports.oreslang_wasm_actor_teardown();
try{handle(0n);throw Error('post-teardown execution')}catch(e){if(!(e instanceof WebAssembly.RuntimeError))throw e}
other.exports.oreslang_wasm_actor_teardown();
console.log('SOURCE_ACTOR_WASM32_ISOLATION_PASS');
"""
    result = run([args.node, "--input-type=module", "-e", js, wasm])
    assert result.returncode == 0 and "SOURCE_ACTOR_WASM32_ISOLATION_PASS" in result.stdout, (
        result.stdout, result.stderr
    )
    # A receiver marker is a spelling alias, not an extra guest parameter.
    # Compile the *real* alternate source to Wasm and execute both instances.
    explicit = Path(folder) / "inline-self.ores"
    original = Path(args.fixture).read_text(encoding="utf8")
    assert "on_message(" in original, "source fixture has no actor handler"
    explicit.write_text(
        original.replace("on_message(", "on_message(self, ", 1),
        encoding="utf8")
    explicit_ir = run([args.compiler, "--target=wasm32", str(explicit)])
    assert explicit_ir.returncode == 0 and not explicit_ir.stderr, (
        explicit_ir.stderr, explicit_ir.stdout)
    assert "@oreslang_wasm_actor_on_message(i64" in explicit_ir.stdout
    assert "oreslang_source_actor_spawn" not in explicit_ir.stdout
    alternate_wasm = str(Path(folder) / "inline-self.wasm")
    alternate_link = run([
        args.clang, "--target=wasm32", "-x", "ir", "-Oz", "-nostdlib",
        "-Wl,--no-entry",
        "-Wl,--export=oreslang_wasm_actor_init",
        "-Wl,--export=oreslang_wasm_actor_on_message",
        "-Wl,--export=oreslang_wasm_actor_teardown",
        "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
        "-o", alternate_wasm, "-"
    ], input=explicit_ir.stdout)
    assert alternate_link.returncode == 0, alternate_link.stderr
    alternate_result = run([args.node, "--input-type=module", "-e", js, alternate_wasm])
    assert (alternate_result.returncode == 0 and
            "SOURCE_ACTOR_WASM32_ISOLATION_PASS" in alternate_result.stdout), (
                alternate_result.stdout, alternate_result.stderr)

    # The initial guest ABI is strictly one actor type, one host-controlled instance.
    invalid = Path(folder) / "two.ores"
    invalid.write_text(Path(args.fixture).read_text() +
                       Path(args.fixture).read_text().replace("Counter", "Another"),
                       encoding="utf8")
    denied = run([args.compiler, "--target=wasm32", str(invalid)])
    assert denied.returncode != 0 and not denied.stdout
    assert ("exactly one source actor" in denied.stderr or
            "one actor per instance" in denied.stderr), denied.stderr
print("REAL_ORESLANG_WASM32_ACTOR_ABI_PASS")
