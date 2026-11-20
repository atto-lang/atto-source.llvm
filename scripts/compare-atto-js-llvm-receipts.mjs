#!/usr/bin/env node
// Compares separately produced JS/LLVM checked-i64 receipt *contents*.
// This is NOT independent runner attestation. Do not turn this into a
// 'cross-backend verified' receipt without independent stepful job evidence.
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';

const CORPUS_URL = new URL('../contracts/atto-parity-v1/js-checked-i64-candidates.json',import.meta.url);
const CORPUS_SHA='5fd8ed522dfaaead8dc1b2d780159a4f156482124b7515d0a788245fc594ebc8';
const sha256=bytes=>createHash('sha256').update(bytes).digest('hex');
const hash64=value=>typeof value==='string'&&/^[a-f0-9]{64}$/.test(value);
const commit40=value=>typeof value==='string'&&/^[a-f0-9]{40}$/.test(value);
const trap=value=>typeof value==='string'&&
  /^(Illegal instruction|Trace\/breakpoint trap|Abort trap)$/.test(value);
function requireTruth(value,reason){if(!value)throw Error(reason);}
export function compareReceipts(js,llvm,manifest) {
  requireTruth(js&&typeof js==='object'&&llvm&&typeof llvm==='object'&&
    manifest&&Array.isArray(manifest.cases),'missing comparison input');
  requireTruth(js.format==='oreslang-js-program-v2-node-execution-v1'&&
    js.scope==='source-local-node-esm-only'&&
    js.cross_backend_parity_verified===false,'JS source receipt profile mismatch');
  requireTruth(js.expected_cases===50&&js.passed_cases===50&&js.failed_cases===0&&
    Array.isArray(js.results)&&js.results.length===50 &&
    hash64(js.manifest_sha256)&&js.manifest_sha256===CORPUS_SHA &&
    commit40(js.git_head)&&commit40(js.git_tree)&&
    /^v22\.|^v2[3-9]\.|^v[3-9][0-9]\./.test(js.node_version),
    'JS result inventory, compiler profile or identity incomplete');
  requireTruth(llvm.schema==='atto-llvm-checked-i64-native-v1'&&
    llvm.verification==='LLVM_NATIVE_GUEST_EXECUTED'&&
    commit40(llvm.source_head)&&commit40(llvm.source_tree)&&
    llvm.corpus_sha256===CORPUS_SHA&&
    hash64(llvm.compiler_sha256)&&hash64(llvm.clang_sha256)&&
    llvm.equality_control_guests===4 &&
    llvm.values_checked===37&&llvm.overflow_guests_trapped===13 &&
    llvm.executed_guest_processes===104&&
    JSON.stringify(llvm.optimization_modes)===JSON.stringify(['O0','O2']) &&
    Array.isArray(llvm.results)&&llvm.results.length===50,
    'LLVM native result inventory, compiler profile or identity incomplete');
  requireTruth(manifest.schemaVersion===1&&
    manifest.integerModel==='signed-i64-checked-provisional'&&
    manifest.cases.length===50,'canonical source manifest schema mismatch');
  const ids=new Set();let values=0,overflows=0;
  const prefix='// oreslang:checked-i64-v1\npub fnc main(): int {return ';
  const suffix=';}\n';
  for(let i=0;i<50;i++){
    const spec=manifest.cases[i],j=js.results[i],l=llvm.results[i];
    requireTruth(spec&&j&&l&&typeof spec.id==='string'&&
      !ids.has(spec.id),'missing, duplicate or out-of-order parity case');
    ids.add(spec.id);
    requireTruth(typeof spec.source==='string' &&
      spec.source.startsWith(prefix)&&spec.source.endsWith(suffix),
      'unexpected source program structure at '+spec.id);
    const expected=spec.expected;
    const originalHash=sha256(Buffer.from(spec.source,'utf8'));
    const expression=spec.source.slice(prefix.length,-suffix.length);
    const nativeSource=expected.kind==='value'
      ? 'pub fnc main(): int {\n  if (('+expression+') eq ('+expected.value+')) { return 42; } fi\n  return 43;\n}\n'
      : spec.source;
    requireTruth(j.id===spec.id&&j.source_sha256===originalHash&&
      j.expected_kind===expected.kind&&j.observed_kind===expected.kind&&
      j.expected_value===(expected.value??null)&&
      j.observed_value===(expected.kind==='value'?expected.value:null)&&
      j.pass===true&&hash64(j.emitted_sha256),
      'JS mismatch or missing ESM emission at '+spec.id);
    requireTruth(l.id===spec.id&&l.kind===expected.kind&&
      l.original_source_sha256===originalHash&&
      l.native_source_sha256===sha256(Buffer.from(nativeSource,'utf8')),
      'LLVM source identity or wrapper mismatch at '+spec.id);
    if(expected.kind==='value'){
      values++;
      requireTruth(l.native_observed==='value-eq' &&
        l.raw_guest_result==='42' && l.raw_guest_result_o2==='42',
        'LLVM full-width equality mismatch at '+spec.id);
    }else if(expected.kind==='checked-overflow'){
      overflows++;
      requireTruth(l.native_observed==='overflow-trap' &&
        trap(l.raw_guest_result) && trap(l.raw_guest_result_o2),
        'LLVM expected runtime trap missing at '+spec.id);
    }else throw Error('unsupported numeric outcome at '+spec.id);
  }
  requireTruth(values===37&&overflows===13,'unexpected value/overflow count');
  return Object.freeze({
    schema:'atto-js-llvm-checked-i64-receipt-comparison-v1',
    status:'RECEIPT_CONTENT_MATCH_ONLY',
    execution_attested:false,
    cross_backend_parity_verified:false,
    js_head:js.git_head,
    llvm_head:llvm.source_head,
    corpus_sha256:CORPUS_SHA,
    cases_matched:50,values_matched:37,overflow_cases_matched:13,
    optimization_modes:['O0','O2'],
    note:'Receipt contents agree; actual GitHub runner/job steps and native compiler execution require independent authentication.'
  });
}
function main(argv){
  if(argv.length!==2)throw Error('usage: node scripts/compare-atto-js-llvm-receipts.mjs <js-receipt.json> <llvm-receipt.json>');
  const corpusBytes=readFileSync(CORPUS_URL);
  requireTruth(sha256(corpusBytes)===CORPUS_SHA,'canonical source manifest was modified');
  const js=JSON.parse(readFileSync(argv[0],'utf8'));
  const llvm=JSON.parse(readFileSync(argv[1],'utf8'));
  process.stdout.write(JSON.stringify(compareReceipts(js,llvm,JSON.parse(corpusBytes)),null,2)+'\n');
}
if(process.argv[1]&&fileURLToPath(import.meta.url)===resolve(process.argv[1])){
  try{main(process.argv.slice(2));}catch(error){
    console.error('Fail-closed receipt comparison: '+error.message);
    process.exitCode=1;
  }
}
