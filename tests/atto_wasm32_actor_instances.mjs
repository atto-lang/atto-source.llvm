// Executable negative-lifecycle and sibling-containment checks on a REAL
// wasm32 actor module. The caller must supply compiler-produced bytes; this
// file does not synthesize a fake AttoLang compiler result.
//
// For the Counter fixture, per-actor Wasm instances have independent globals.
// A Wasm Instance memory boundary is not an OS process or tenant sandbox.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';

export function verifyAttoWasmActorInstances(wasmBytes) {
  assert.ok(WebAssembly.validate(wasmBytes), 'invalid source-compiled wasm32 module');
  const module = new WebAssembly.Module(wasmBytes);
  assert.deepEqual(WebAssembly.Module.imports(module), [],
    'compiler guest must not import ambient host capabilities');
  const create = () => new WebAssembly.Instance(module).exports;
  const a = create(), b = create();
  for (const exports of [a, b]) {
    assert.equal(typeof exports.oreslang_wasm_actor_init, 'function');
    assert.equal(typeof exports.oreslang_wasm_actor_on_message, 'function');
    assert.equal(typeof exports.oreslang_wasm_actor_teardown, 'function');
  }
  assert.throws(() => a.oreslang_wasm_actor_on_message(1n),
    WebAssembly.RuntimeError, 'uninitialized actor can run');
  assert.throws(() => b.oreslang_wasm_actor_on_message(1n),
    WebAssembly.RuntimeError, 'uninitialized sibling can run');

  a.oreslang_wasm_actor_init();
  b.oreslang_wasm_actor_init();
  assert.equal(a.oreslang_wasm_actor_on_message(1n), 1n);
  assert.equal(a.oreslang_wasm_actor_on_message(2n), 3n);
  assert.equal(b.oreslang_wasm_actor_on_message(7n), 7n,
    'sibling inherited another actor heap or globals');
  assert.equal(a.oreslang_wasm_actor_on_message(3n), 6n,
    'another actor overwrote active actor state');
  assert.throws(() => a.oreslang_wasm_actor_init(),
    WebAssembly.RuntimeError, 'double init was not rejected');
  assert.equal(b.oreslang_wasm_actor_on_message(1n), 8n,
    'invalid neighbor call poisoned sibling');

  a.oreslang_wasm_actor_teardown();
  assert.throws(() => a.oreslang_wasm_actor_on_message(9n),
    WebAssembly.RuntimeError, 'retired actor resumed');
  assert.throws(() => a.oreslang_wasm_actor_teardown(),
    WebAssembly.RuntimeError, 'repeated teardown accepted');
  assert.equal(b.oreslang_wasm_actor_on_message(1n), 9n,
    'retiring actor A altered live actor B');
  b.oreslang_wasm_actor_teardown();

  const restarted = create();
  restarted.oreslang_wasm_actor_init();
  assert.equal(restarted.oreslang_wasm_actor_on_message(5n), 5n,
    'new actor inherited retired actor memory');
  restarted.oreslang_wasm_actor_teardown();

  // Negative binary mutation: even if bytes are corrupted after compilation,
  // the runtime must not treat the altered binary as an admitted guest.
  const malformed = Uint8Array.from(wasmBytes);
  // WebAssembly magic starts with 0x00: writing zero is a NO-OP.
  malformed[0] = 0xff;
  assert.equal(WebAssembly.validate(malformed), false,
    'malformed compiled Wasm was admitted');
  assert.throws(() => new WebAssembly.Module(malformed),
    WebAssembly.CompileError);
  const another = create();
  another.oreslang_wasm_actor_init();
  assert.equal(another.oreslang_wasm_actor_on_message(4n), 4n,
    'malformed neighboring guest poisoned still-valid module');
  another.oreslang_wasm_actor_teardown();

  return {
    kind: 'source-compiled-wasm32-node-actor-instance-check',
    assertion_profile: 'private instance state; lifecycle traps; malformed binary rejection; sibling recovery',
    wasm_sha256: createHash('sha256').update(wasmBytes).digest('hex'),
    result: 'PASS'
  };
}

// Optional standalone mode for independently generated .wasm files.
// Must not print PASS when the module is absent, malformed or ABI-incompatible.
if (process.argv[1] &&
    fileURLToPath(import.meta.url) === process.argv[1]) {
  if (process.argv.length !== 3) {
    throw Error('usage: node atto_wasm32_actor_instances.mjs compiled-actor.wasm');
  }
  console.log(JSON.stringify(verifyAttoWasmActorInstances(readFileSync(process.argv[2]))));
}
