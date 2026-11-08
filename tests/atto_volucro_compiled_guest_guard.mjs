// Fail-closed verification of compiled actor ABI and executable probe receipts.
// Local, untrusted Wasm bytes have no host imports or ambient capabilities.
import assert from 'node:assert/strict';

export const MAX_ACTOR_WASM_BYTES = 1024 * 1024;
const ACTOR = ['oreslang_wasm_actor_init', 'oreslang_wasm_actor_on_message',
  'oreslang_wasm_actor_teardown'];
const PASS = 'ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS A=[1,3,6] B=[7,8] A_restart=[5]';
const FAIL = 'ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_FAIL:';
const MAGIC = Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]);
function deny(message) { throw Error('ACTOR_WASM_ADMISSION_DENIED: ' + message); }
function u32(data, p, end) {
  let value = 0;
  for (let i = 0; i < 5; ++i) {
    if (p.pos >= end) deny('truncated u32 LEB');
    const b = data[p.pos++];
    if (i === 4 && (b & 0xf0) !== 0) deny('u32 LEB overflow');
    value += (b & 0x7f) * 2 ** (7 * i);
    if ((b & 0x80) === 0) return value;
  }
  deny('unterminated u32 LEB');
}
function byte(data, p, end) {
  if (p.pos >= end) deny('truncated Wasm byte');
  return data[p.pos++];
}
function array(data, p, end, countLimit = 4096) {
  const count = u32(data, p, end);
  if (count > countLimit) deny('excessive Wasm count');
  return count;
}
function name(data, p, end) {
  const n = u32(data, p, end);
  if (n > 512 || p.pos + n > end) deny('invalid export name');
  const s = new TextDecoder('utf-8', {fatal: true}).decode(data.subarray(p.pos, p.pos + n));
  p.pos += n;
  return s;
}
function sectionsOf(input) {
  // Check length BEFORE copying untrusted bytes into a new Buffer.
  if (!(input instanceof Uint8Array) || input.byteLength > MAX_ACTOR_WASM_BYTES)
    deny('compiled Wasm must be a bounded byte array');
  const data = Buffer.from(input);
  if (data.length < 8 || !data.subarray(0, 8).equals(MAGIC))
    deny('missing Wasm magic or oversized module');
  if (!WebAssembly.validate(data)) deny('invalid compiled Wasm');
  const module = new WebAssembly.Module(data);
  if (WebAssembly.Module.imports(module).length !== 0) deny('ambient host import');
  const sections = new Map();
  const p = {pos: 8};
  while (p.pos < data.length) {
    const id = byte(data, p, data.length);
    const size = u32(data, p, data.length);
    const end = p.pos + size;
    if (end > data.length) deny('truncated Wasm section');
    if (id !== 0) {
      if (sections.has(id)) deny('duplicate Wasm section');
      sections.set(id, {start: p.pos, end});
    }
    if (id === 8) deny('implicit Wasm start not authorized');
    if (id === 2) deny('Wasm import section not authorized');
    p.pos = end;
  }
  return {data, module, sections};
}
function requireSection(sections, id) {
  const section = sections.get(id);
  if (!section) deny('required Wasm section missing: ' + id);
  return section;
}
function typesOf(data, section) {
  const p = {pos: section.start};
  const count = array(data, p, section.end);
  const types = [];
  for (let i = 0; i < count; ++i) {
    if (byte(data, p, section.end) !== 0x60) deny('unsupported nonfunction type');
    const params = [];
    for (let n = array(data, p, section.end, 512); n > 0; --n)
      params.push(byte(data, p, section.end));
    const results = [];
    for (let n = array(data, p, section.end, 512); n > 0; --n)
      results.push(byte(data, p, section.end));
    types.push({params, results});
  }
  if (p.pos !== section.end) deny('type section trailing bytes');
  return types;
}
function functionTypesOf(data, section) {
  const p = {pos: section.start};
  const count = array(data, p, section.end);
  const indexes = [];
  for (let i = 0; i < count; ++i) indexes.push(u32(data, p, section.end));
  if (p.pos !== section.end) deny('function section trailing bytes');
  return indexes;
}
function exportsOf(data, section) {
  const p = {pos: section.start};
  const count = array(data, p, section.end);
  const exports = new Map();
  for (let i = 0; i < count; ++i) {
    const n = name(data, p, section.end);
    const kind = byte(data, p, section.end);
    const index = u32(data, p, section.end);
    if (exports.has(n)) deny('duplicate export');
    exports.set(n, {kind, index});
  }
  if (p.pos !== section.end) deny('export section trailing bytes');
  return exports;
}
function memoryOf(data, section) {
  const p = {pos: section.start};
  if (array(data, p, section.end) !== 1) deny('expected exactly one private memory');
  const flags = u32(data, p, section.end);
  // Exactly min/max ordinary wasm32 linear memory. Excludes shared/memory64.
  if (flags !== 1) deny('shared, memory64, or unbounded actor memory');
  const min = u32(data, p, section.end);
  const max = u32(data, p, section.end);
  if (min < 1 || min > 2 || max > 2 || max < min)
    deny('actor memory must have a bounded 1-2 page profile');
  if (p.pos !== section.end) deny('memory section trailing bytes');
  return {min, max};
}
export function assertCompiledActorProfile(bytes) {
  const {data, module, sections} = sectionsOf(bytes);
  if (sections.has(4)) deny('wasm tables not admitted for scalar actor');
  const memory = memoryOf(data, requireSection(sections, 5));
  const types = typesOf(data, requireSection(sections, 1));
  const functions = functionTypesOf(data, requireSection(sections, 3));
  const exports = exportsOf(data, requireSection(sections, 7));
  const expected = [[[], []], [[0x7e], [0x7e]], [[], []]];
  for (let i = 0; i < ACTOR.length; ++i) {
    const entry = exports.get(ACTOR[i]);
    if (!entry || entry.kind !== 0 || entry.index >= functions.length)
      deny('missing/invalid actor function export: ' + ACTOR[i]);
    const type = types[functions[entry.index]];
    if (!type || type.params.length !== expected[i][0].length ||
        type.results.length !== expected[i][1].length ||
        type.params.some((v, j) => v !== expected[i][0][j]) ||
        type.results.some((v, j) => v !== expected[i][1][j]))
      deny('wrong actor function ABI: ' + ACTOR[i]);
  }
  // wasm-ld exports `memory` for this scalar ABI; additional guest-visible
  // functions, memories, globals and tables are not admitted by this profile.
  for (const [name, entry] of exports) {
    if (ACTOR.includes(name)) continue;
    if (name === 'memory' && entry.kind === 2 && entry.index === 0) continue;
    deny('unexpected actor export: ' + name);
  }
  return {memory, module};
}
export function assertInjectedOutOfBounds(wasm, kind) {
  const {module} = sectionsOf(wasm);
  if (!['read', 'write'].includes(kind)) deny('unknown mutation kind');
  const fn = new WebAssembly.Instance(module).exports.oreslang_wasm_actor_on_message;
  if (typeof fn !== 'function') deny('mutated on_message function missing');
  assert.throws(() => fn(1n), err => err instanceof WebAssembly.RuntimeError &&
    /memory access out of bounds/i.test(err.message),
    'injected ' + kind + ' must trigger a real memory out-of-bounds trap');
}
export function assertVmOutcome(result, expected, label) {
  assert.equal(result.signal ?? null, null, label + ' must not be killed by signal');
  if (expected === 'positive') {
    assert.equal(result.status, 0, label + ' must exit successfully');
    assert.equal(result.stdout.trim(), PASS, label + ' actual guest values missing');
    assert.ok(!result.stderr.includes(FAIL), label + ' reports a guest failure');
    return;
  }
  if (!['MissingExport', 'ExecutionTrapped'].includes(expected))
    throw Error('unsupported typed guest failure expected');
  assert.equal(result.status, 1, label + ' must be a typed host error, not build/tool failure');
  assert.equal(result.stdout.trim(), '', label + ' emitted success output on failure');
  // The Rust binary is invoked directly, never through a Cargo wrapper.
  // Every stderr byte must belong to the single expected typed error line.
  // A planted marker in an unrelated tool failure cannot become a PASS.
  assert.equal(result.stderr.trim(), FAIL + ' tenant A: Wasm(' + expected + ')',
    label + ' has extra, forged, missing, or misclassified Rust guest stderr');
}

// Bind a Cargo build to its actual emitted executable, rather than using
// 'cargo run' as both a build tool and a negative guest exit-code oracle.
export function selectCargoGuestExecutable(stdout, targetName) {
  if (typeof stdout !== 'string' || typeof targetName !== 'string')
    deny('invalid Cargo artifact transcript');
  const paths = new Set();
  let finished = false;
  for (const line of stdout.split(/\r?\n/)) {
    if (!line.trim()) continue;
    let record;
    try { record = JSON.parse(line); }
    catch { deny('invalid Cargo JSON artifact transcript'); }
    if (record.reason === 'build-finished') {
      if (finished || record.success !== true) deny('incomplete or failed Cargo build');
      finished = true;
    }
    if (record.reason === 'compiler-artifact' && record.target?.name === targetName &&
        Array.isArray(record.target?.kind) && record.target.kind.includes('bin')) {
      if (finished || typeof record.executable !== 'string' ||
          !record.executable.startsWith('/') || record.executable.includes('\0'))
        deny('invalid Cargo executable artifact');
      paths.add(record.executable);
    }
  }
  if (!finished || paths.size !== 1) deny('Cargo build did not identify one exact guest executable');
  return [...paths][0];
}
