#!/usr/bin/env node
// SOURCE-FREE lower-tier C -> LLVM IR -> Wasm32 linker profile canary.
// Not AttoLang frontend compilation, Volucro Wasmi or OS isolation evidence.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtempSync, readFileSync, rmSync, statSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {assertCompiledActorProfile} from './atto_volucro_compiled_guest_guard.mjs';

const root = dirname(fileURLToPath(import.meta.url));
const source = join(root, 'atto_volucro_compiled_guest_linker_canary.c');
const clang = process.env.ATTO_CLANG || 'clang';
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
function exec(args) {
  const r = spawnSync(clang, args, {
    cwd: root, encoding: 'utf8', timeout: 120000, maxBuffer: 4 * 1024 * 1024,
    env: {...process.env, LC_ALL: 'C'},
  });
  if (r.error || r.signal || r.status !== 0)
    throw Error('C/LLVM-IR/Wasm compiler failed: ' + String(r.error || r.signal || r.stderr).slice(0, 1000));
  return r.stdout;
}
const tmp = mkdtempSync(join(tmpdir(), 'atto-wasm-ir-canary-'));
try {
  const irFile = join(tmp, 'actor.ll');
  const wasmFile = join(tmp, 'actor.wasm');
  const version = exec(['--version']).split('\n')[0];
  exec(['--target=wasm32', '-S', '-emit-llvm', '-O1', '-ffreestanding', '-fno-builtin',
    '-o', irFile, source]);
  const ir = readFileSync(irFile, 'utf8');
  assert.match(ir, /target triple = "wasm32(?:-unknown-unknown)?"/);
  for (const entry of ['oreslang_wasm_actor_init', 'oreslang_wasm_actor_on_message',
    'oreslang_wasm_actor_teardown']) {
    assert.ok(ir.includes('@' + entry + '('), 'missing LLVM IR entrypoint: ' + entry);
  }
  exec(['--target=wasm32', '-x', 'ir', '-Oz', '-nostdlib', '-Wl,--no-entry',
    '-Wl,--initial-memory=131072', '-Wl,--max-memory=131072',
    '-Wl,--export=oreslang_wasm_actor_init',
    '-Wl,--export=oreslang_wasm_actor_on_message',
    '-Wl,--export=oreslang_wasm_actor_teardown', '-o', wasmFile, irFile]);
  const metadata = statSync(wasmFile);
  assert.ok(metadata.isFile() && metadata.size > 0 && metadata.size <= 1024 * 1024,
    'unbounded Wasm module');
  const bytes = readFileSync(wasmFile);
  const profile = assertCompiledActorProfile(bytes);
  assert.deepEqual(profile.memory, {min: 2, max: 2});
  const m = new WebAssembly.Module(bytes);
  assert.deepEqual(WebAssembly.Module.imports(m), []);
  const actor = () => new WebAssembly.Instance(m).exports;
  const a = actor(), b = actor();
  for (const x of [a, b]) x.oreslang_wasm_actor_init();
  assert.equal(a.oreslang_wasm_actor_on_message(1n), 1n);
  assert.equal(a.oreslang_wasm_actor_on_message(2n), 3n);
  assert.equal(b.oreslang_wasm_actor_on_message(7n), 7n);
  assert.equal(a.oreslang_wasm_actor_on_message(3n), 6n);
  assert.equal(b.oreslang_wasm_actor_on_message(1n), 8n);
  a.oreslang_wasm_actor_teardown();
  b.oreslang_wasm_actor_teardown();
  const replacement = actor();
  replacement.oreslang_wasm_actor_init();
  assert.equal(replacement.oreslang_wasm_actor_on_message(5n), 5n);
  replacement.oreslang_wasm_actor_teardown();
  console.log(JSON.stringify({
    status: 'ATTO_WASM32_LLVM_IR_LINKER_CANARY_ONLY_PASS',
    evidence_class: 'C_to_LLVM_IR_to_Wasm32_Node_V8_only',
    not_proved: ['AttoLang source compilation', 'Volucro Wasmi guest execution',
      'same-VM tenant containment', 'native code sandboxing'],
    clang_version: version, source_sha256: sha256(readFileSync(source)),
    ir_sha256: sha256(Buffer.from(ir)), wasm_sha256: sha256(bytes),
    memory: profile.memory, outputs: {A: [1, 3, 6], B: [7, 8], A_restart: [5]},
  }));
} finally {
  rmSync(tmp, {recursive: true, force: true});
}
