#!/usr/bin/env node
// Cross-project, exact-head fault injection against an actual AttoLang-compiled
// wasm32 actor and Volucro's real Wasmi guest CLI. All inputs are local only.
// This is NOT source-level exploit generation or an OS sandbox certification.
import assert from 'node:assert/strict';
import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, isAbsolute, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {mutateActorWasmExport, mutateActorWasmOutOfBounds, wasmSha256} from './atto_volucro_compiled_guest_faults_lib.mjs';

const options = new Map();
for(let i=2;i<process.argv.length;i+=2){
  const key=process.argv[i], value=process.argv[i+1];
  if(!key?.startsWith('--') || !value || options.has(key))
    throw Error('expected unique --key value pairs');
  options.set(key,value);
}
const required=['--compiler','--clang','--cargo','--volucro-root','--expected-llvm-head','--expected-volucro-head'];
for(const key of required) if(!options.has(key)) throw Error('missing '+key);
for(const key of options.keys()) if(!required.includes(key)) throw Error('unknown '+key);
for(const key of ['--compiler','--clang','--cargo','--volucro-root'])
  if(!isAbsolute(options.get(key))) throw Error(key+' requires an absolute path');
const llvmRoot=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const volucroRoot=resolve(options.get('--volucro-root'));
const fixture=join(llvmRoot,'tests/fixtures/wasm32-single-source-actor.ores');

function command(program,args,{cwd,input}={}) {
  const result=spawnSync(program,args,{cwd,input,encoding:'utf8',timeout:120000,maxBuffer:16*1024*1024});
  if(result.error || result.signal || result.status===null)
    throw Error('command unavailable or timed out: '+program+': '+String(result.error||result.signal));
  return result;
}
function mustPass(program,args,options={}){
  const r=command(program,args,options);
  if(r.status!==0) throw Error(program+' failed, exit='+r.status+': '+r.stderr.slice(0,1000));
  return r.stdout;
}
function head(root,expected,name){
  if(!/^[0-9a-f]{40}$/.test(expected)) throw Error(name+' SHA must be full lowercase 40-hex');
  assert.equal(mustPass('git',['rev-parse','HEAD'],{cwd:root}).trim(),expected,name+' source head mismatch');
  assert.equal(mustPass('git',['status','--porcelain=v1','--untracked-files=all'],{cwd:root}).trim(),'','dirty '+name+' source checkout');
  return expected;
}
const llvmHead=head(llvmRoot,options.get('--expected-llvm-head'),'LLVM');
const volucroHead=head(volucroRoot,options.get('--expected-volucro-head'),'Volucro');
const source=readFileSync(fixture);
const ir=mustPass(options.get('--compiler'),['--target=wasm32',fixture]);
assert.match(ir,/target triple = "wasm32-unknown-unknown"/);
for(const entry of ['oreslang_wasm_actor_init','oreslang_wasm_actor_on_message','oreslang_wasm_actor_teardown'])
  assert.ok(ir.includes('@'+entry+'('),'real compiler output missing '+entry);
assert.ok(!ir.includes('declare void @oreslang_source_actor_'),'unexpected native actor host ABI import');

const temp=mkdtempSync(join(tmpdir(),'atto-volucro-negative-'));
function runVm(file){
  return command(options.get('--cargo'),['run','--quiet','--manifest-path',join(volucroRoot,'Cargo.toml'),
    '-p','vm-core','--bin','atto_actor_isolation_smoke','--',file],{cwd:volucroRoot});
}
const positive=/^ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS A=\[1,3,6\] B=\[7,8\] A_restart=\[5\]$/;
function expectPositive(file,label){
  const result=runVm(file);
  assert.equal(result.status,0,label+' must actually run the guest successfully: '+result.stderr.slice(-1000));
  assert.match(result.stdout.trim(),positive,label+' canonical output not demonstrated');
  assert.ok(!result.stderr.includes('ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_FAIL'),label+' reported failure');
}
function expectNegative(file,expected,label){
  const result=runVm(file);
  assert.notEqual(result.status,0,label+' must be rejected by Volucro Wasmi');
  assert.match(result.stderr,/ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_FAIL:/,label+' expected typed VM denial, not missing cargo/tool');
  assert.match(result.stderr,expected,label+' must fail for the intended security reason');
  assert.ok(!result.stdout.includes('ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS'),label+' printed false PASS');
}
try {
  const original=join(temp,'compiled-actor.wasm');
  mustPass(options.get('--clang'),['--target=wasm32','-x','ir','-Oz','-nostdlib',
    '-Wl,--no-entry','-Wl,--initial-memory=131072','-Wl,--max-memory=131072',
    '-Wl,--export=oreslang_wasm_actor_init','-Wl,--export=oreslang_wasm_actor_on_message',
    '-Wl,--export=oreslang_wasm_actor_teardown','-o',original,'-'],{input:ir});
  const baseline=readFileSync(original);
  assert.ok(WebAssembly.validate(baseline),'real compiler produced invalid wasm');
  assert.deepEqual(WebAssembly.Module.imports(new WebAssembly.Module(baseline)),[],
    'real compiler output unexpectedly has imports');
  const abi=mutateActorWasmExport(baseline);
  const fault=mutateActorWasmOutOfBounds(baseline);
  const abiFile=join(temp,'mutated-missing-abi.wasm');
  const faultFile=join(temp,'mutated-oob-i64-load.wasm');
  writeFileSync(abiFile,abi,{mode:0o600});
  writeFileSync(faultFile,fault,{mode:0o600});
  // A successful initial guest run is mandatory; failure mutations must not
  // pass when the original compiler/runtime path was never functional.
  expectPositive(original,'baseline');
  expectNegative(abiFile,/Wasm\(MissingExport\)/,'renamed compiled on_message export');
  expectNegative(faultFile,/Wasm\(ExecutionTrapped\)/,'out-of-bounds compiled guest load');
  // This recovery is a NEW VM invocation, not evidence of same-VM sibling
  // survival. Separate Volucro vm-core tests must prove same-VM survival.
  expectPositive(original,'recovery control');
  console.log(JSON.stringify({status:'COMPILED_WASM_MUTATION_GUEST_EXECUTED',
    evidence_scope:'actual AttoLang LLVM compiler -> Wasm mutation -> Volucro Wasmi, process recovery only',
    not_proved:['source-level malicious actor','same-VM sibling survival','OS process sandbox','native LLVM arbitrary-pointer isolation'],
    llvm_head:llvmHead,volucro_head:volucroHead,
    source_sha256:wasmSha256(source),compiler_sha256:wasmSha256(readFileSync(options.get('--compiler'))),
    original_wasm_sha256:wasmSha256(baseline),missing_abi_sha256:wasmSha256(abi),oob_load_sha256:wasmSha256(fault),
    probes:{baseline:'PASS',abi_mutation:'EXPECTED_MISSING_EXPORT',out_of_bounds:'EXPECTED_EXECUTION_TRAPPED',recovery:'PASS'}}));
} finally {
  rmSync(temp,{recursive:true,force:true});
}
