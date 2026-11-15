import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {compareReceipts} from '../scripts/compare-atto-js-llvm-receipts.mjs';

// These are deliberately SYNTHETIC receipt-shape tests. The constructed
// receipts below are not source compiler execution or runner attestations.
const sha=s=>createHash('sha256').update(s).digest('hex');
const corpus=JSON.parse(readFileSync(new URL('../contracts/atto-parity-v1/js-checked-i64-candidates.json',import.meta.url)));
const prefix='// oreslang:checked-i64-v1\npub fnc main(): int {return ';
const suffix=';}\n';
const head='1'.repeat(40),tree='2'.repeat(40),digest='3'.repeat(64);
function mock(){
  const expected=corpus.cases.map(item=>{
    const expression=item.source.slice(prefix.length,-suffix.length);
    const emitted=item.expected.kind==='value'
      ? 'pub fnc main(): int {\n  if (('+expression+') eq ('+item.expected.value+')) { return 42; } fi\n  return 43;\n}\n'
      :item.source;
    return {item,emitted};
  });
  const js={
    format:'oreslang-js-program-v2-node-execution-v1',
    scope:'source-local-node-esm-only',
    cross_backend_parity_verified:false,
    expected_cases:50,passed_cases:50,failed_cases:0,
    manifest_sha256:'5fd8ed522dfaaead8dc1b2d780159a4f156482124b7515d0a788245fc594ebc8',
    node_version:'v22.16.0',git_head:head,git_tree:tree,
    results:expected.map(({item})=>({
      id:item.id,source_sha256:sha(item.source),
      expected_kind:item.expected.kind,
      expected_value:item.expected.value??null,
      observed_kind:item.expected.kind,
      observed_value:item.expected.kind==='value'?item.expected.value:null,
      emitted_sha256:digest,pass:true
    }))
  };
  const llvm={
    schema:'atto-llvm-checked-i64-native-v1',
    verification:'LLVM_NATIVE_GUEST_EXECUTED',
    source_head:head,source_tree:tree,
    corpus_sha256:js.manifest_sha256,
    compiler_sha256:digest,clang_sha256:digest,
    optimization_modes:['O0','O2'],
    equality_control_guests:4,executed_guest_processes:104,
    values_checked:37,overflow_guests_trapped:13,
    results:expected.map(({item,emitted})=>({
      id:item.id,kind:item.expected.kind,
      native_observed:item.expected.kind==='value'?'value-eq':'overflow-trap',
      raw_guest_result:item.expected.kind==='value'?'42':'Illegal instruction',
      raw_guest_result_o2:item.expected.kind==='value'?'42':'Illegal instruction',
      original_source_sha256:sha(item.source),native_source_sha256:sha(emitted)
    }))
  };
  return {js,llvm};
}
test('synthetic 50-case match NEVER claims independently verified parity',()=>{
  const {js,llvm}=mock();
  const got=compareReceipts(js,llvm,corpus);
  assert.equal(got.status,'RECEIPT_CONTENT_MATCH_ONLY');
  assert.equal(got.execution_attested,false);
  assert.equal(got.cross_backend_parity_verified,false);
  assert.equal(got.cases_matched,50);
  assert.equal(got.values_matched,37);
  assert.equal(got.overflow_cases_matched,13);
});
for(const [name,mutate,pattern] of [
  ['missing JS guest',({js})=>{js.passed_cases=49;},/JS result inventory/],
  ['forged successful JS outcome',({js})=>{js.results[0].observed_value='9';},/JS mismatch/],
  ['missing JS ESM artifact',({js})=>{js.results[0].emitted_sha256=null;},/JS mismatch/],
  ['different original JS source digest',({js})=>{js.results[0].source_sha256=digest;},/JS mismatch/],
  ['changed LLVM wrapper bytes',({llvm})=>{llvm.results[0].native_source_sha256=digest;},/LLVM source identity/],
  ['missing O2 value execution',({llvm})=>{llvm.results[0].raw_guest_result_o2='43';},/full-width equality mismatch/],
  ['overflow reported as generic exit',({llvm})=>{const x=llvm.results.find(r=>r.kind==='checked-overflow');x.raw_guest_result_o2='1';},/runtime trap missing/],
  ['wrong optimization inventory',({llvm})=>{llvm.optimization_modes=['O0'];},/LLVM native result inventory/],
  ['missing source-sensitivity control',({llvm})=>{llvm.equality_control_guests=2;},/LLVM native result inventory/],
  ['missing compiler identity',({llvm})=>{llvm.compiler_sha256='';},/LLVM native result inventory/],
  ['source revision cannot be authenticated',({js})=>{js.git_head='';},/JS result inventory/],
  ['inconsistent case ordering',({llvm})=>{llvm.results.reverse();},/LLVM source identity/],
  ['duplicate source IDs',(_,{manifest})=>{manifest.cases[1].id=manifest.cases[0].id;},/JS mismatch|duplicate/]
]){
  test('reject synthetic receipt mutation: '+name,()=>{
    const pair=mock();
    const manifest=JSON.parse(JSON.stringify(corpus));
    mutate(pair,{manifest});
    assert.throws(()=>compareReceipts(pair.js,pair.llvm,manifest),pattern);
  });
}
