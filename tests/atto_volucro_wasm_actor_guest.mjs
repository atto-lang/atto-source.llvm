#!/usr/bin/env node
// Actual .ores/.atto source -> LLVM wasm32 IR -> .wasm -> authenticated
// Volucro Wasmi tenant/actor execution. Fails closed on missing tools,
// incorrect source heads, wrong imports, unexecuted guests, or bad output.
// This is a developer smoke test, NOT an OS/hardware sandbox certificate.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve, dirname} from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {verifyAttoWasmActorInstances} from './atto_wasm32_actor_instances.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  const key = process.argv[i], value = process.argv[i + 1];
  if (!key?.startsWith('--') || !value || args.has(key))
    throw Error('expected unique --key value pairs');
  args.set(key, value);
}
const required = ['--compiler', '--clang', '--cargo', '--volucro-root',
  '--expected-llvm-head', '--expected-volucro-head'];
for (const key of required) if (!args.has(key)) throw Error('missing ' + key);
for (const key of args.keys()) if (!required.includes(key))
  throw Error('unknown option ' + key);

const llvmRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const volucroRoot = resolve(args.get('--volucro-root'));
const fixture = join(llvmRoot, 'tests', 'fixtures', 'wasm32-single-source-actor.ores');
const capture = (program, argv, options={}) => {
  const result = spawnSync(program, argv, {
    input: options.input, encoding: 'utf8', timeout: 120_000,
    maxBuffer: 16 * 1024 * 1024, cwd: options.cwd,
  });
  if (result.error || result.status !== 0 || result.signal)
    throw Error('executable ' + program + ' failed (' + result.status
      + '): ' + String(result.error || result.stderr || result.signal));
  return result.stdout;
};
const checkHead = (root, expected, label) => {
  if (!/^[0-9a-f]{40}$/.test(expected)) throw Error(label + ' head must be full SHA');
  const actual = capture('git', ['rev-parse', 'HEAD'], {cwd: root}).trim();
  assert.equal(actual, expected, label + ' source head mismatch');
  const dirty = capture('git', ['status', '--porcelain=v1', '--untracked-files=all'], {cwd: root});
  assert.equal(dirty.trim(), '', label + ' checkout contains uncommitted or untracked source');
  return actual;
};
const llvmHead=checkHead(llvmRoot,args.get('--expected-llvm-head'),'LLVM');
const volucroHead=checkHead(volucroRoot,args.get('--expected-volucro-head'),'Volucro');
const source=readFileSync(fixture);
const ir=capture(args.get('--compiler'),['--target=wasm32',fixture]);
assert.match(ir,/target triple = "wasm32-unknown-unknown"/);
for (const symbol of ['oreslang_wasm_actor_init',
  'oreslang_wasm_actor_on_message','oreslang_wasm_actor_teardown'])
  assert.ok(ir.includes('@' + symbol + '('),'compiled actor ABI missing ' + symbol);
assert.ok(!ir.includes('declare void @oreslang_source_actor_'),
  'native host actor scheduler import forbidden in Wasm guest');

const folder=mkdtempSync(join(tmpdir(),'atto-volucro-wasm32-'));
try {
  const wasmFile=join(folder,'actor.wasm');
  capture(args.get('--clang'),['--target=wasm32','-x','ir','-Oz','-nostdlib',
    '-Wl,--no-entry','-Wl,--initial-memory=131072','-Wl,--max-memory=131072',
    '-Wl,--export=oreslang_wasm_actor_init',
    '-Wl,--export=oreslang_wasm_actor_on_message',
    '-Wl,--export=oreslang_wasm_actor_teardown',
    '-o',wasmFile,'-'],{input:ir});
  const wasm=readFileSync(wasmFile);
  assert.ok(WebAssembly.validate(wasm),'actual compiled Wasm invalid');
  const module=new WebAssembly.Module(wasm);
  assert.deepEqual(WebAssembly.Module.imports(module),[],
    'generated guest unexpectedly requests host imports');
  // Source-compiled native-Wasm negative lifecycle and sibling evidence.
  // The Volucro Wasmi exercise below is separate and mandatory.
  const nodeActorProof = verifyAttoWasmActorInstances(wasm);
  const output=capture(args.get('--cargo'),['run','--quiet','--manifest-path',
    join(volucroRoot,'Cargo.toml'),'-p','vm-core',
    '--bin','atto_actor_isolation_smoke','--',wasmFile],{cwd:volucroRoot});
  assert.match(output,/ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS/);
  const sha256=bytes=>createHash('sha256').update(bytes).digest('hex');
  // Recording the actual compiler binary SHA is independent of the source
  // commit; matching this hash to a reproducible build is a later gate.
  const compilerSha=sha256(readFileSync(args.get('--compiler')));
  console.log(JSON.stringify({
    result:'real LLVM AttoLang wasm32 source -> Volucro Wasmi guest',
    llvm_head:llvmHead, volucro_head:volucroHead,
    fixture_sha256:sha256(source), wasm_sha256:sha256(wasm),
    compiler_binary_sha256:compilerSha,
    guest_execution:output.trim(), node_actor_proof:nodeActorProof,
    evidence_level:'guest-executed',
    scope:'per actor Wasmi Store + trusted-host DomainId (not OS process sandbox)'
  }));
} finally {
  rmSync(folder,{recursive:true,force:true});
}
