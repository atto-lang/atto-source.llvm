// Execute actual compiled guest Wasm, not a parser mock.
// Usage (from frontend/): cargo build --locked --bin attoc
//    node tests/match-return-guest.mjs
// Optional native-vs-wasm32 compiler byte parity:
//    ATTO_COMPILER_WASM=target/wasm32-unknown-unknown/release/atto_compiler.wasm node tests/match-return-guest.mjs
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = fileURLToPath(new URL('.', import.meta.url));
const attoc = resolve(process.env.ATTOC || join(here, '../target/debug/attoc'));
const tmp = mkdtempSync(join(tmpdir(), 'atto-match-'));
const decoder = new TextDecoder('utf-8', { fatal: true });
let wasmCompiler = null;
if (process.env.ATTO_COMPILER_WASM) {
  const module = new WebAssembly.Module(readFileSync(process.env.ATTO_COMPILER_WASM));
  assert.deepEqual(WebAssembly.Module.imports(module), [], 'compiler must have zero imports');
  wasmCompiler = new WebAssembly.Instance(module, {}).exports;
  assert.equal(wasmCompiler.abi_version(), 2, 'compiler ABI');
}

function compileWasm(source) {
  const bytes = new TextEncoder().encode(source);
  assert.ok(bytes.length <= wasmCompiler.input_capacity(), 'compiler input capacity');
  new Uint8Array(wasmCompiler.memory.buffer, wasmCompiler.input_ptr(), bytes.length).set(bytes);
  const rc = wasmCompiler.compile(bytes.length);
  if (rc !== 0) {
    assert.equal(wasmCompiler.result_length(), 0, 'no guest artifact on wasm32 compile error');
    return { ok: false };
  }
  return { ok: true, wasm: Buffer.from(
    new Uint8Array(wasmCompiler.memory.buffer, wasmCompiler.result_ptr(),
      wasmCompiler.result_length()).slice()) };
}

function guestRun(wasm) {
  assert.ok(WebAssembly.validate(wasm), 'guest binary must be valid Wasm');
  const module = new WebAssembly.Module(wasm);
  let stdout = '';
  let instance;
  const allowed = new Set(['print_i64', 'print_bool', 'print_str', 'print_newline', 'print_f64',
    'actor_spawn', 'actor_send', 'state_register', 'state_enter', 'state_leave']);
  for (const item of WebAssembly.Module.imports(module)) {
    assert.equal(item.module, 'atto', 'guest may import only the atto host namespace');
    assert.ok(allowed.has(item.name), 'unexpected host capability: ' + item.name);
  }
  instance = new WebAssembly.Instance(module, { atto: {
    print_i64: value => { stdout += String(value); },
    print_bool: value => { stdout += value ? 'true' : 'false'; },
    print_str: (ptr, len) => {
      stdout += decoder.decode(new Uint8Array(instance.exports.memory.buffer, ptr, len));
    },
    print_newline: () => { stdout += '\n'; },
    print_f64: value => { stdout += String(value); },
    // Known ABI imports may exist in the module but the match fixture must not
    // actually invoke any actor or state capability.
    actor_spawn: () => { throw new Error('unexpected actor spawn in match guest'); },
    actor_send: () => { throw new Error('unexpected actor send in match guest'); },
    state_register: () => { throw new Error('unexpected state registration in match guest'); },
    state_enter: () => { throw new Error('unexpected critical-state operation'); },
    state_leave: () => { throw new Error('unexpected critical-state operation'); },
  }});
  const value = instance.exports.main();
  assert.equal(typeof value, 'bigint', 'main must return int64');
  assert.equal(instance.exports.panic_code.value, 0, 'guest panic code');
  return { exit: Number(value), stdout };
}

try {
  const cases = [
    ['pass/match-guard-block-return.atto', { exit: 0, stdout: '2\n2\n' }],
    ['pass/match-guard-block-caret.atto', { exit: 0, stdout: '2\n2\n' }],
    ['pass/adt-option-guard-hit.atto', { exit: 21, stdout: '' }],
    ['pass/adt-result-ok-guard.atto', { exit: 42, stdout: '' }],
    ['pass/adt-guard-skips-wrong-result-tag.atto', { exit: 42, stdout: '' }],
    ['pass/adt-whole-value-guard-binding.atto', { exit: 42, stdout: '' }],
    ['pass/adt-result-err-discard.atto', { exit: 42, stdout: '' }],
    ['pass/adt-result-ok-discard.atto', { exit: 42, stdout: '' }],
    ['pass/adt-option-some-discard.atto', { exit: 42, stdout: '' }],
    ['fail/adt-discard-duplicate-tag.atto', { error: 'E_MATCH' }],
    ['fail/adt-discard-none-arity.atto', { error: 'E_ARITY' }],
    ['fail/match-guard-no-fallback.atto', { error: 'E_MATCH' }],
    ['fail/match-guard-block-fallthrough.atto', { error: 'E_RETURN' }],
  ];
  for (const [relative, expectation] of cases) {
    const input = join(here, 'fixtures', relative);
    const output = join(tmp, relative.replaceAll('/', '_') + '.wasm');
    const result = spawnSync(attoc, [input, '-o', output], { encoding: 'utf8', timeout: 60_000 });
    assert.equal(result.error, undefined, 'attoc process failed to launch');
    const source = readFileSync(input, 'utf8');
    if (expectation.error) {
      assert.notEqual(result.status, 0, relative + ' must reject');
      assert.match(result.stderr, new RegExp(expectation.error + ':'),
        relative + ' must use stable diagnostic');
      assert.ok(!existsSync(output), relative + ' must not leave guest artifact');
      if (wasmCompiler) assert.equal(compileWasm(source).ok, false,
        relative + ': wasm32 compiler must reject');
    } else {
      assert.equal(result.status, 0, relative + ': ' + result.stderr);
      const nativeGuest = readFileSync(output);
      const actual = guestRun(nativeGuest);
      assert.deepEqual(actual, expectation, relative + ': guest output/exit differs');
      if (wasmCompiler) {
        const other = compileWasm(source);
        assert.ok(other.ok, relative + ': wasm32 compiler rejected');
        assert.deepEqual(other.wasm, nativeGuest, relative + ': compiled bytes differ');
        assert.deepEqual(guestRun(other.wasm), expectation, relative + ': wasm32 guest differs');
      }
    }
  }
  console.log('PASS: 13/13 exact-match and ADT discard contracts executed by frontend; real guest stdout/exit and no-artifact proofs');
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
