#!/usr/bin/env node
// Cross-project, exact-head fault injection against an actual AttoLang-compiled
// wasm32 actor and Volucro's real Wasmi guest CLI. All inputs are local only.
// This is NOT source-level exploit generation or an OS sandbox certification.
import assert from 'node:assert/strict';
import {readFileSync, writeFileSync, mkdtempSync, rmSync, statSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, isAbsolute, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {mutateActorWasmExport, mutateActorWasmOutOfBounds, mutateActorWasmOutOfBoundsStore, wasmSha256} from './atto_volucro_compiled_guest_faults_lib.mjs';
import {assertCompiledActorProfile, assertInjectedOutOfBounds, assertVmOutcome, MAX_ACTOR_WASM_BYTES} from './atto_volucro_compiled_guest_guard.mjs';

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
function expectPositive(file,label){
  assertVmOutcome(runVm(file),'positive',label);
}
function expectNegative(file,expected,label){
  assertVmOutcome(runVm(file),expected,label);
}
try {
  const original=join(temp,'compiled-actor.wasm');
  mustPass(options.get('--clang'),['--target=wasm32','-x','ir','-Oz','-nostdlib',
    '-Wl,--no-entry','-Wl,--initial-memory=131072','-Wl,--max-memory=131072',
    '-Wl,--export=oreslang_wasm_actor_init','-Wl,--export=oreslang_wasm_actor_on_message',
    '-Wl,--export=oreslang_wasm_actor_teardown','-o',original,'-'],{input:ir});
  const generated=statSync(original);
  assert.ok(generated.isFile() && generated.size>0 && generated.size<=MAX_ACTOR_WASM_BYTES,
    'compiler-generated Wasm exceeds Volucro 1 MiB admission budget');
  const baseline=readFileSync(original);
  assert.ok(WebAssembly.validate(baseline),'real compiler produced invalid wasm');
  assert.deepEqual(WebAssembly.Module.imports(new WebAssembly.Module(baseline)),[],
    'real compiler output unexpectedly has imports');
  // Validate the exact scalar AttoLang actor ABI and bounded two-page memory
  // BEFORE injecting any faults or allowing a runtime PASS classification.
  const compiledProfile=assertCompiledActorProfile(baseline);
  const abi=mutateActorWasmExport(baseline);
  const fault=mutateActorWasmOutOfBounds(baseline);
  const storeFault=mutateActorWasmOutOfBoundsStore(baseline);
  assert.throws(()=>assertCompiledActorProfile(abi),/missing\/invalid actor function export/,
    'ABI mutation did not actually revoke the required handler');
  assertInjectedOutOfBounds(fault,'read');
  assertInjectedOutOfBounds(storeFault,'write');
  const abiFile=join(temp,'mutated-missing-abi.wasm');
  const faultFile=join(temp,'mutated-oob-i64-load.wasm');
  const storeFaultFile=join(temp,'mutated-oob-i64-store.wasm');
  writeFileSync(abiFile,abi,{mode:0o600});
  writeFileSync(faultFile,fault,{mode:0o600});
  writeFileSync(storeFaultFile,storeFault,{mode:0o600});
  // A successful initial guest run is mandatory; failure mutations must not
  // pass when the original compiler/runtime path was never functional.
  expectPositive(original,'baseline');
  expectNegative(abiFile,'MissingExport','renamed compiled on_message export');
  expectNegative(faultFile,'ExecutionTrapped','out-of-bounds compiled guest load');
  expectNegative(storeFaultFile,'ExecutionTrapped','out-of-bounds compiled guest store');
  // This recovery is a NEW VM invocation, not evidence of same-VM sibling
  // survival. Separate Volucro vm-core tests must prove same-VM survival.
  expectPositive(original,'recovery control');
  console.log(JSON.stringify({status:'COMPILED_WASM_MUTATION_GUEST_EXECUTED',
    evidence_scope:'actual AttoLang LLVM compiler -> Wasm mutation -> Volucro Wasmi, process recovery only',
    not_proved:['source-level malicious actor','same-VM sibling survival','OS process sandbox','native LLVM arbitrary-pointer isolation'],
    llvm_head:llvmHead,volucro_head:volucroHead,node_engine_version:process.version,
    source_sha256:wasmSha256(source),compiler_sha256:wasmSha256(readFileSync(options.get('--compiler'))),
    validated_actor_memory_pages:compiledProfile.memory,
    original_wasm_sha256:wasmSha256(baseline),missing_abi_sha256:wasmSha256(abi),oob_load_sha256:wasmSha256(fault),oob_store_sha256:wasmSha256(storeFault),
    probes:{baseline:'PASS',abi_mutation:'EXPECTED_MISSING_EXPORT',oob_load:'EXPECTED_EXECUTION_TRAPPED',oob_store:'EXPECTED_EXECUTION_TRAPPED',recovery:'PASS'}}));
} finally {
  rmSync(temp,{recursive:true,force:true});
}
