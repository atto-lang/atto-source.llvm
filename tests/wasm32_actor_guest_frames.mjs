#!/usr/bin/env node
// Exact compiler-generated .atto -> LLVM IR -> Clang -> Node Wasm acceptance.
// Distinguish executed guest from IR-only and host-only checks in CI records.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const flags = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  if (!['--compiler', '--clang', '--fixture'].includes(process.argv[i]) ||
      i + 1 >= process.argv.length || flags.has(process.argv[i]))
    throw Error('usage: test.mjs --compiler PATH --clang PATH --fixture .atto');
  flags.set(process.argv[i], process.argv[i + 1]);
}
for (const flag of ['--compiler', '--clang', '--fixture'])
  if (!flags.has(flag)) throw Error('missing ' + flag);

const run = (program, args, options = {}) => {
  const r = spawnSync(program, args, {...options, encoding: 'utf8', timeout: 90000,
    maxBuffer: 4 * 1024 * 1024});
  if (r.error || r.status !== 0)
    throw Error('failed ' + program + ' ' + args.join(' ') + '\n' +
      (r.error?.message || r.stderr || r.stdout));
  return r.stdout;
};
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const ir = run(flags.get('--compiler'),
  ['--target=wasm32', resolve(flags.get('--fixture'))]);
// Test actual compiler-emitted IR: one actor has a fixed 2-cell scalar frame.
assert.match(ir, /@atto\.wasm\.actor\.scalar\.frame\.v1\s*=\s*internal global\s*\[2 x i64\] zeroinitializer/);
assert.match(ir, /getelementptr inbounds \[2 x i64\]/);
assert.ok((ir.match(/store volatile i64/g) || []).length >= 4,
  'message, field and terminal frame scrubbing must be volatile guest stores');
assert.doesNotMatch(ir, /%message\.slot\s*=\s*alloca/,
  'message frame must not silently fall back to engine stack');
assert.doesNotMatch(ir, /declare .*@oreslang_source_actor_/,
  'unapproved runtime/host imports');
const output = mkdtempSync(join(tmpdir(), 'atto-wasm-guest-scalar-frame-'));
try {
  for (const optimization of ['-O0', '-Oz']) {
    const destination = join(output,
      'actor' + optimization.slice(1).toLowerCase() + '.wasm');
    run(flags.get('--clang'), [
      '--target=wasm32', '-x', 'ir', optimization, '-nostdlib',
      '-Wl,--no-entry', '-Wl,--export-memory',
      '-Wl,--export=oreslang_wasm_actor_init',
      '-Wl,--export=oreslang_wasm_actor_on_message',
      '-Wl,--export=oreslang_wasm_actor_teardown',
      '-Wl,--initial-memory=131072', '-Wl,--max-memory=131072',
      '-o', destination, '-'
    ], {input: ir});
    const wasm = readFileSync(destination);
    assert.equal(WebAssembly.validate(wasm), true);
    const module = new WebAssembly.Module(wasm);
    assert.deepEqual(WebAssembly.Module.imports(module), []);
    const exports = WebAssembly.Module.exports(module).map(x => x.name).sort();
    assert.deepEqual(exports, ['memory', 'oreslang_wasm_actor_init',
      'oreslang_wasm_actor_on_message', 'oreslang_wasm_actor_teardown']);
    const a = new WebAssembly.Instance(module, {}).exports;
    const b = new WebAssembly.Instance(module, {}).exports;
    assert.notStrictEqual(a.memory, b.memory);
    assert.notStrictEqual(a.memory.buffer, b.memory.buffer);
    assert.equal(a.memory.buffer.byteLength, 131072);
    assert.throws(() => a.memory.grow(1), RangeError);
    assert.throws(() => a.oreslang_wasm_actor_on_message(1n), WebAssembly.RuntimeError);
    a.oreslang_wasm_actor_init();
    b.oreslang_wasm_actor_init();
    // The actor state may itself occupy private linear-memory globals.
    // Static compiler-generated IR verifies frame scrub stores separately;
    // requiring the whole actor heap to stay unchanged would be incorrect.
    for (const [actor, initial, amount] of [
      [a, 0n, 0x1234567890n], [b, 0n, 5n], [a, 0x1234567890n, 7n],
      [b, 5n, 1n]
    ]) {
      assert.equal(actor.oreslang_wasm_actor_on_message(amount), initial + amount);
    }
    assert.equal(a.oreslang_wasm_actor_on_message(-7n), 0x1234567890n);
    a.oreslang_wasm_actor_teardown();
    assert.throws(() => a.oreslang_wasm_actor_on_message(1n), WebAssembly.RuntimeError);
    assert.equal(b.oreslang_wasm_actor_on_message(2n), 8n);
    b.oreslang_wasm_actor_teardown();
    console.log('EXECUTED_ATTO_GUEST_WASM_FRAME_PASS', optimization,
      'source_sha256=' + sha(readFileSync(flags.get('--fixture'))),
      'ir_sha256=' + sha(Buffer.from(ir)),
      'wasm_sha256=' + sha(wasm));
  }
} finally {
  rmSync(output, {recursive: true, force: true});
}
