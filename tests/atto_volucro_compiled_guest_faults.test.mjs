import test from 'node:test';
import assert from 'node:assert/strict';
import {mutateActorWasmExport, mutateActorWasmOutOfBounds, wasmSha256} from './atto_volucro_compiled_guest_faults_lib.mjs';
const encodeU32 = number => {
  const out=[];do {let b=number%128;number=Math.floor(number/128);if(number)b|=128;out.push(b);}while(number);
  return out;
};
const str = s => [...encodeU32(Buffer.byteLength(s)),...Buffer.from(s)];
const section=(id,payload)=>[id,...encodeU32(payload.length),...payload];
function moduleBytes(){
  const types=section(1,[2,0x60,0,0,0x60,1,0x7e,1,0x7e]);
  const functions=section(3,[3,0,1,0]);
  const memory=section(5,[1,1,1,1]);
  const exports=section(7,[3,...str('oreslang_wasm_actor_init'),0,0,...str('oreslang_wasm_actor_on_message'),0,1,...str('oreslang_wasm_actor_teardown'),0,2]);
  const code=section(10,[3,2,0,0x0b,4,0,0x20,0,0x0b,2,0,0x0b]);
  return Buffer.from([0,97,115,109,1,0,0,0,...types,...functions,...memory,...exports,...code]);
}
const valid=moduleBytes();
test('synthetic Node wasm fixture is valid and runnable',()=>{
  assert.equal(WebAssembly.validate(valid),true);
  const m=new WebAssembly.Module(valid);const instance=new WebAssembly.Instance(m);
  instance.exports.oreslang_wasm_actor_init();
  assert.equal(instance.exports.oreslang_wasm_actor_on_message(17n),17n);
  instance.exports.oreslang_wasm_actor_teardown();
});
test('export mutation keeps valid wasm but denies required typed actor ABI',()=>{
  const mutated=mutateActorWasmExport(valid);
  assert.equal(WebAssembly.validate(mutated),true);
  const exports=new WebAssembly.Instance(new WebAssembly.Module(mutated)).exports;
  assert.equal(exports.oreslang_wasm_actor_on_message,undefined);
  assert.equal(typeof exports.oreslang_wasm_actor_on_messagf,'function');
  assert.notEqual(wasmSha256(valid),wasmSha256(mutated));
});
test('out-of-bounds injection keeps valid wasm but traps on actual i64.load',()=>{
  const mutated=mutateActorWasmOutOfBounds(valid);
  assert.equal(WebAssembly.validate(mutated),true);
  const instance=new WebAssembly.Instance(new WebAssembly.Module(mutated));
  assert.throws(()=>instance.exports.oreslang_wasm_actor_on_message(19n),WebAssembly.RuntimeError);
  const sibling=new WebAssembly.Instance(new WebAssembly.Module(valid));
  assert.equal(sibling.exports.oreslang_wasm_actor_on_message(9n),9n);
  assert.notEqual(wasmSha256(valid),wasmSha256(mutated));
});
test('deterministic fault bytes and digests',()=>{
  assert.equal(wasmSha256(mutateActorWasmOutOfBounds(valid)),wasmSha256(mutateActorWasmOutOfBounds(valid)));
});
test('module without on_message export is not mutation-authorized',()=>{
  const missing=mutateActorWasmExport(valid);
  assert.throws(()=>mutateActorWasmOutOfBounds(missing),/missing compiled on_message/);
});
test('malformed truncated magic and source never accepted',()=>{
  for(const m of [Buffer.alloc(0),Buffer.from('not wasm'),valid.subarray(0,15)]){
    assert.throws(()=>mutateActorWasmOutOfBounds(m),/WASM_MUTATION_DENIED/);
    assert.throws(()=>mutateActorWasmExport(m),/WASM_MUTATION_DENIED/);
  }
});
test('failed guest does not mutate baseline bytes',()=>{
  const before=wasmSha256(valid);
  mutateActorWasmOutOfBounds(valid); mutateActorWasmExport(valid);
  assert.equal(wasmSha256(valid),before);
});
test('an import-bearing guest is denied before any mutation',()=>{
  // Insert a function import in canonical section order. The resulting module
  // remains valid; the mutation helper rejects imports without trusting IDs.
  const importSection=section(2,[1,...str('env'),...str('read_secret'),0,0]);
  const firstFunction=valid.indexOf(Buffer.from([3,4,3,0,1,0]),8);
  assert.ok(firstFunction>0);
  const imported=Buffer.concat([valid.subarray(0,firstFunction),Buffer.from(importSection),valid.subarray(firstFunction)]);
  assert.equal(WebAssembly.validate(imported),true);
  assert.equal(WebAssembly.Module.imports(new WebAssembly.Module(imported)).length,1);
  assert.throws(()=>mutateActorWasmExport(imported),/host imports forbidden/);
  assert.throws(()=>mutateActorWasmOutOfBounds(imported),/host imports forbidden/);
});
test('out-of-bounds injection fails closed without any declared linear memory',()=>{
  const memoryBytes=Buffer.from(section(5,[1,1,1,1]));
  const i=valid.indexOf(memoryBytes);
  assert.ok(i>0);
  const noMemory=Buffer.concat([valid.subarray(0,i),valid.subarray(i+memoryBytes.length)]);
  assert.equal(WebAssembly.validate(noMemory),true);
  assert.throws(()=>mutateActorWasmOutOfBounds(noMemory),/mutation invalidated wasm/);
});
test('export mutation does not introduce host imports or change original module',()=>{
  const output=mutateActorWasmExport(valid);
  assert.deepEqual(WebAssembly.Module.imports(new WebAssembly.Module(output)),[]);
  assert.equal(typeof new WebAssembly.Instance(new WebAssembly.Module(valid)).exports.oreslang_wasm_actor_on_message,'function');
});
