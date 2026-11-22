import test from 'node:test';
import assert from 'node:assert/strict';
import {assertCompiledActorProfile, assertInjectedOutOfBounds, assertVmOutcome, selectCargoGuestExecutable,
  MAX_ACTOR_WASM_BYTES} from './atto_volucro_compiled_guest_guard.mjs';
const u32 = n => {const r=[];do{let b=n&127;n=Math.floor(n/128);if(n)b|=128;r.push(b);}while(n);return r;};
const str = s => [...u32(Buffer.byteLength(s)),...Buffer.from(s)];
const sec = (id, p) => [id,...u32(p.length),...p];
const names = ['oreslang_wasm_actor_init','oreslang_wasm_actor_on_message','oreslang_wasm_actor_teardown'];
const binary = (opts={}) => {
  const types = sec(1, [2,0x60,0,0,0x60,1,opts.wrongType ? 0x7f : 0x7e,1,0x7e]);
  const funcs = sec(3, [3,0,1,0]);
  const memory = opts.noMemory ? [] : sec(5, [1,opts.memoryFlags ?? 1,opts.memoryMin ?? 1,opts.memoryMax ?? 1]);
  const exports = sec(7,[3,...names.flatMap((n,i)=>[...str(n),0,i])]);
  const handler = opts.fault === 'read'
    ? [0,0x41,0x7f,0x29,0,0,0x1a,0x20,0,0x0b]
    : opts.fault === 'write'
      ? [0,0x41,0x7f,0x42,0x2a,0x37,0,0,0x20,0,0x0b]
      : opts.fault === 'unreachable' ? [0,0x00,0x20,0,0x0b]
      : opts.wrongType ? [0,0x20,0,0xad,0x0b] : [0,0x20,0,0x0b];
  const code = sec(10,[3,2,0,0x0b,...u32(handler.length),...handler,2,0,0x0b]);
  return Buffer.from([0,97,115,109,1,0,0,0,...types,...funcs,...memory,...exports,...code]);
};
const PASS='ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS A=[1,3,6] B=[7,8] A_restart=[5]';
const FAIL='ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_FAIL:';
const good = binary();
test('valid bounded actor ABI is admitted',()=>{
  assert.equal(WebAssembly.validate(good),true);
  assert.deepEqual(assertCompiledActorProfile(good).memory,{min:1,max:1});
});
test('two-page scalar actor profile matches linker reservation',()=>{
  const wasm=binary({memoryMin:2,memoryMax:2});
  assert.equal(WebAssembly.validate(wasm),true);
  assert.deepEqual(assertCompiledActorProfile(wasm).memory,{min:2,max:2});
});
test('valid Wasm table section is rejected by scalar actor profile',()=>{
  const tableSection=Buffer.from(sec(4,[1,0x70,1,1,1]));
  const startOfMemory=good.indexOf(Buffer.from(sec(5,[1,1,1,1])));
  assert.ok(startOfMemory>0);
  const wasm=Buffer.concat([good.subarray(0,startOfMemory),tableSection,good.subarray(startOfMemory)]);
  assert.equal(WebAssembly.validate(wasm),true);
  assert.throws(()=>assertCompiledActorProfile(wasm),/wasm tables not admitted/);
});
test('legitimate wasm-ld memory export is allowed but extraneous exports are denied',()=>{
  // The exact LLVM-IR linker can export one bounded `memory` alongside the
  // three actor lifecycle functions; no arbitrary exports should be accepted.
  function appendExport(bytes, name, kind, index) {
    let p=8;
    while(p<bytes.length) {
      const start=p, id=bytes[p++];
      let length=0, shift=0, b;
      do { b=bytes[p++];length+=(b&127)*2**shift;shift+=7; } while(b&128);
      const end=p+length;
      if(id===7) {
        const payload=bytes.subarray(p,end);
        assert.equal(payload[0],3, 'fixture exports three actor entrypoints');
        const changed=Buffer.from(sec(7,[4,...payload.subarray(1),...str(name),kind,index]));
        return Buffer.concat([bytes.subarray(0,start),changed,bytes.subarray(end)]);
      }
      p=end;
    }
    throw Error('fixture export section missing');
  }
  const memoryExport=appendExport(good,'memory',2,0);
  assert.equal(WebAssembly.validate(memoryExport),true);
  assertCompiledActorProfile(memoryExport);
  const unexpected=appendExport(good,'read_secret',0,1);
  assert.equal(WebAssembly.validate(unexpected),true);
  assert.throws(()=>assertCompiledActorProfile(unexpected),/unexpected actor export: read_secret/);
  const extraMemory=appendExport(good,'other_memory',2,0);
  assert.equal(WebAssembly.validate(extraMemory),true);
  assert.throws(()=>assertCompiledActorProfile(extraMemory),/unexpected actor export: other_memory/);
});
test('mismatched i32 actor handler ABI is rejected',()=>{
  const bad=binary({wrongType:true});
  assert.equal(WebAssembly.validate(bad),true);
  assert.throws(()=>assertCompiledActorProfile(bad),/wrong actor function ABI/);
});
test('zero-memory guest is rejected as a nonconforming actor',()=>{
  const bad=binary({noMemory:true});
  assert.equal(WebAssembly.validate(bad),true);
  assert.throws(()=>assertCompiledActorProfile(bad),/required Wasm section missing/);
});
test('memory64, shared or unbounded memory profiles are denied',()=>{
  for(const flag of [0,3,5,7]){
    const m=binary({memoryFlags:flag});
    assert.throws(()=>assertCompiledActorProfile(m),/ACTOR_WASM_ADMISSION_DENIED/);
  }
});
test('more than two bounded memory pages are denied',()=>{
  for(const m of [binary({memoryMin:3,memoryMax:3}),binary({memoryMin:1,memoryMax:3})]){
    assert.throws(()=>assertCompiledActorProfile(m),/bounded 1-2 page profile/);
  }
});
test('oversize binary denied before Wasm compilation',()=>{
  const m=Buffer.alloc(MAX_ACTOR_WASM_BYTES+1);
  m.set(good);
  assert.throws(()=>assertCompiledActorProfile(m),/bounded byte array/);
});
test('explicit start function is denied even though baseline would otherwise validate',()=>{
  // Function 0 is type ()->(), so a valid Wasm start section can invoke it.
  const insert=good.length-sec(10,[3,2,0,0x0b,4,0,0x20,0,0x0b,2,0,0x0b]).length;
  assert.ok(insert>0);
  const m=Buffer.concat([good.subarray(0,insert),Buffer.from(sec(8,[0])),good.subarray(insert)]);
  assert.equal(WebAssembly.validate(m),true);
  assert.throws(()=>assertCompiledActorProfile(m),/implicit Wasm start/);
});
test('read and write mutation probes require actual memory-out-of-bounds traps',()=>{
  assertInjectedOutOfBounds(binary({fault:'read'}),'read');
  assertInjectedOutOfBounds(binary({fault:'write'}),'write');
  assert.throws(()=>assertInjectedOutOfBounds(good,'read'),/injected read/);
});
test('unreachable trap cannot masquerade as memory out-of-bounds',()=>{
  const bad = binary({fault:'unreachable'});
  assert.equal(WebAssembly.validate(bad),true);
  assert.throws(()=>assertInjectedOutOfBounds(bad,'read'),/injected read/);
});
test('missing real actor function export is rejected even when binary is valid',()=>{
  const forged = Buffer.from(good);
  const offset = forged.indexOf(Buffer.from('oreslang_wasm_actor_on_message'));
  assert.ok(offset >= 0);
  forged[offset+'oreslang_wasm_actor_on_message'.length-1] = 'f'.charCodeAt(0);
  assert.equal(WebAssembly.validate(forged),true);
  assert.throws(()=>assertCompiledActorProfile(forged),/missing\/invalid actor function export/);
});
test('typed Wasmi success requires exact complete stdout',()=>{
  assertVmOutcome({status:0,signal:null,stdout:PASS+'\n',stderr:''},'positive','baseline');
  assert.throws(()=>assertVmOutcome({status:0,stdout:PASS+'\n'+PASS,stderr:''},'positive','fake'),/actual guest values/);
});
test('typed failure requires exact reason and exit status 1',()=>{
  assertVmOutcome({status:1,stdout:'',stderr:FAIL+' tenant A: Wasm(ExecutionTrapped)\n'},'ExecutionTrapped','oob');
  assertVmOutcome({status:1,stdout:'',stderr:FAIL+' tenant A: Wasm(MissingExport)\n'},'MissingExport','abi');
  for(const status of [0,101,127,null]){
    assert.throws(()=>assertVmOutcome({status,stdout:'',stderr:FAIL+' tenant A: Wasm(ExecutionTrapped)\n'},'ExecutionTrapped','fake'));
  }
});
test('unrelated tool failure with planted trap substring is denied',()=>{
  const bad=[
    'linker failed: '+FAIL+' tenant A: Wasm(ExecutionTrapped)',
    FAIL+' tenant B: Wasm(ExecutionTrapped)',
    FAIL+' tenant A: Wasm(MissingExport)',
    FAIL+' tenant A: Wasm(ExecutionTrapped)\n'+FAIL+' tenant A: Wasm(ExecutionTrapped)',
  ];
  for(const stderr of bad){
    assert.throws(()=>assertVmOutcome({status:1,stdout:'',stderr},'ExecutionTrapped','fake'));
  }
});
test('guest failure accompanied by unrelated stderr cannot forge a typed denial',()=>{
  const marker=FAIL+' tenant A: Wasm(ExecutionTrapped)';
  for(const noise of [
    marker+'\nlinker failed',
    'fatal: missing binary\n'+marker,
    marker+'\n    at runCommand',
  ]) assert.throws(()=>assertVmOutcome({status:1,stdout:'',stderr:noise},'ExecutionTrapped','spoof'),
      /extra, forged, missing, or misclassified/);
});
test('negative guest never writes positive receipt to stdout',()=>{
  assert.throws(()=>assertVmOutcome({status:1,stdout:PASS,stderr:FAIL+' tenant A: Wasm(ExecutionTrapped)'},'ExecutionTrapped','fake'));
});
test('guest killed by signal cannot be treated as typed failure',()=>{
  assert.throws(()=>assertVmOutcome({status:1,signal:'SIGKILL',stdout:'',stderr:FAIL+' tenant A: Wasm(ExecutionTrapped)'},'ExecutionTrapped','fake'));
});
test('unrecognized outcome classifier is rejected',()=>{
  assert.throws(()=>assertVmOutcome({status:1,stdout:'',stderr:''},'SomeOtherError','fake'));
});

test('Cargo artifact selector binds a successful build to one real binary',()=>{
  const good=JSON.stringify({reason:'compiler-artifact',target:{name:'atto_actor_isolation_smoke',kind:['bin']},executable:'/private/volucro/target/debug/atto_actor_isolation_smoke'});
  const ignored=JSON.stringify({reason:'compiler-artifact',target:{name:'some_dependency',kind:['lib']},executable:null});
  const done=JSON.stringify({reason:'build-finished',success:true});
  assert.equal(selectCargoGuestExecutable([ignored,good,done].join('\n'),'atto_actor_isolation_smoke'),
    '/private/volucro/target/debug/atto_actor_isolation_smoke');
});
test('Cargo artifact selector rejects untrusted or incomplete tool transcripts',()=>{
  const artifact=(file)=>JSON.stringify({reason:'compiler-artifact',target:{name:'atto_actor_isolation_smoke',kind:['bin']},executable:file});
  const done=JSON.stringify({reason:'build-finished',success:true});
  for(const bad of [
    '',
    artifact('/a'),
    done,
    [artifact('relative/file'),done].join('\n'),
    [artifact('/a'),JSON.stringify({reason:'build-finished',success:false})].join('\n'),
    [artifact('/a'),artifact('/b'),done].join('\n'),
    [artifact('/a'),done,done].join('\n'),
    [done,artifact('/a')].join('\n'),
    [artifact('/a'),'{ broken-json ',done].join('\n'),
  ]) assert.throws(()=>selectCargoGuestExecutable(bad,'atto_actor_isolation_smoke'),/ACTOR_WASM_ADMISSION_DENIED/);
});
