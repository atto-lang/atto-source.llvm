import assert from 'node:assert/strict';
import {readFileSync, readdirSync, realpathSync, statSync, mkdtempSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';

const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const inputs=[
  ["fail/adt-enum-missing-variant.atto", "57499a8f7f346f9ea16a03a8f71eab5cb8ab3546"],
  ["fail/adt-enum-other-family-arm.atto", "377cd5925554ca0d2dcdc1ac0eba57579fb1804a"],
  ["fail/adt-guard-must-be-bool.atto", "ead6b189d8ff854c3e2bff6bb4be94548343f989"],
  ["fail/adt-nested-wrong-success-payload.atto", "2caff7da4095d30cbfd9e7f4b9be3512cbe505a7"],
  ["fail/adt-option-bool-unsupported.atto", "f2f6e7d0a060726123f0cc003ee36dad50c9b999"],
  ["fail/adt-option-duplicate-unguarded.atto", "325f9f42ee699524bb83ba597bba2c06f15def7c"],
  ["fail/adt-option-guard-not-exhaustive.atto", "8b416255ebdefb9841d5debab4301ad1e19d5673"],
  ["fail/adt-option-wrong-family-pattern.atto", "f4ab0066e858d7c346ce11ccd7d63e5e3d224b08"],
  ["fail/adt-option-wrong-payload-type.atto", "f121a168954a9624f632c19b3139926910e6bc24"],
  ["fail/adt-result-int-error-unsupported.atto", "2438ae98a587d8e0d73711148c9e31a5420a3bab"],
  ["fail/adt-result-wrong-error-type.atto", "d8e04f029d44279e1750282ecd6c5af892f5e864"],
  ["fail/adt-result-wrong-family-pattern.atto", "65bd7797f0480cba910c0478474554133601e461"],
  ["pass/adt-enum-three-way-exhaustive.atto", "2cda216dd4c2dbfce20bdb6d9af0a0cd8cc28415"],
  ["pass/adt-err-guard-fallthrough.atto", "5c882ca6263885d644e8a54cf50ff67a0cbf275c"],
  ["pass/adt-guard-skips-none-payload.atto", "90a5c0beb06ec6f5787ca67d66f555eb14784386"],
  ["pass/adt-guard-skips-wrong-result-tag.atto", "4730792d63852a885c62642c2f79facf8956ad04"],
  ["pass/adt-match-subject-once-guard.atto", "ab3cde2ce9145d07f1403e4d6a06aeb1bd44e6eb"],
  ["pass/adt-nested-err-guard-no-flatten.atto", "03ed7876c3196f53e3dae905b098d0d57b3a014f"],
  ["pass/adt-nested-guard-typed-payload.atto", "0268d620f32d822689f887ea5cb425d356370531"],
  ["pass/adt-option-guard-fallthrough.atto", "8e6f7bf4ba3bd98822c2e7d54395c337372ab914"],
  ["pass/adt-option-guard-hit.atto", "7fb872ba97236df6e1a6b83df55f215665486fad"],
  ["pass/adt-result-err-guard.atto", "c356b6dced659610db9b60eabebca86f4b4aae10"],
  ["pass/adt-result-ok-guard.atto", "ee40cd523d2def9352ad43b8661949334756a9b3"],
  ["pass/adt-whole-value-guard-binding.atto", "4bffdd8d9b828acb907988495e4ef303aa384d2e"]
];
const gitBlob=bytes=>createHash('sha1').update('blob '+bytes.length+'\0').update(bytes).digest('hex');
const sha256=bytes=>createHash('sha256').update(bytes).digest('hex');
const sourceHead='e2d18c418c9d46dde3e7bf4b1936d39139ffefee';
const sourceTree='24035e0b26d52b56c770792cc7d6771e98262ec3';
function run(program,args,{timeout=90000,env=process.env}={}){
 const r=spawnSync(program,args,{cwd:root,encoding:'utf8',timeout,maxBuffer:8*1024*1024,env});
 if(r.error||r.signal||r.status===null)throw Error('tool failed to execute: '+program+' '+String(r.error||r.signal));
 return r;
}
function checkInputs(){
 assert.equal(inputs.length,24);
 assert.equal(inputs.filter(([name])=>name.startsWith('pass/')).length,12);
 assert.equal(inputs.filter(([name])=>name.startsWith('fail/')).length,12);
 const ids=new Set();
 for(const [name,hash] of inputs){
  assert.match(name,/^(?:pass|fail)\/adt-[a-z0-9-]+\.atto$/);
  assert.equal(ids.has(name),false);ids.add(name);
  const file=join(root,'tests/fixtures/adt-guard-v1',name);
  assert.equal(statSync(file).isFile(),true);
  const bytes=readFileSync(file);
  assert.ok(bytes.length>0 && bytes.length<=1_048_576);
  assert.equal(gitBlob(bytes),hash,'source blob drift: '+name);
  const text=bytes.toString('utf8');
  assert.match(text,name.startsWith('pass/')?/\/\/ expect-exit:/g:/\/\/ expect-error:/g);
 }
 for(const kind of ['pass','fail']){
  assert.deepEqual(readdirSync(join(root,'tests/fixtures/adt-guard-v1',kind)).sort(),
   inputs.filter(([name])=>name.startsWith(kind+'/')).map(([name])=>name.split('/').at(-1)).sort());
 }
}
checkInputs();
console.log('LLVM_ADT_GUARD_EXACT_SOURCE_VERIFIED=24 source_head='+sourceHead+' source_tree='+sourceTree);
if(process.argv.includes('--integrity-only'))process.exit(0);
if(!process.env.ORESLANG_LLVMC)throw Error('real compiled oreslang-llvmc path is REQUIRED');
const compiler=realpathSync(process.env.ORESLANG_LLVMC);
assert.equal(statSync(compiler).isFile(),true);
assert.equal(compiler.split('/').at(-1),'oreslang-llvmc');
const runnerDir=mkdtempSync(join(tmpdir(),'atto-llvm-adt-guard-'));
const results=[];
try {
 for(const [name,blob] of inputs){
  const file=join(root,'tests/fixtures/adt-guard-v1',name);
  const source=readFileSync(file,'utf8');
  const positive=name.startsWith('pass/');
  const cr=run(compiler,['--target=host',file],{timeout:90000});
  const diagnostic=(cr.stderr||'').slice(0,2000).replace(/\s+/g,' ');
  if(cr.status!==0){
    assert.equal(cr.stdout.trim(),'','failed LLVM compile leaked partial IR: '+name);
    const state=/unsupported|unimplemented|not supported|not implemented|syntax|parse|unexpected/i.test(diagnostic)
     ?'UNSUPPORTED_SOURCE_NOT_PARITY':'REJECTED_DIAGNOSTIC_NOT_CERTIFIED';
    results.push({id:name,kind:positive?'positive':'negative',blob,compiler_status:cr.status,
      state,semantic_negative_certified:false,guest_executed:false,diagnostic});
    console.log('LLVM_ADT_GUARD_CASE='+name+' state='+state+' compiler_exit='+cr.status);
    continue;
  }
  if(!positive)throw Error('Invalid ADT guest compiled without semantic rejection: '+name);
  assert.match(cr.stdout,/define\s+/,'compiler admitted '+name+' without emitted LLVM function');
  const exitMatch=source.match(/^\/\/ expect-exit:\s*(-?\d+)/m);
  assert.ok(exitMatch,'positive has no expected guest exit: '+name);
  const expected=Number(exitMatch[1]);
  assert.ok(Number.isSafeInteger(expected),'invalid fixture expected return');
  const stdoutMatch=source.match(/^\/\/ expect-stdout:\s*(.*)$/m);
  const expectedOutput=stdoutMatch?stdoutMatch[1]:'';
  const executions=[];
  for(const opt of [0,2]){
   const bin=join(runnerDir,name.replace(/\//g,'-')+'-O'+opt);
   const native=run(join(root,'scripts/compile-aot.sh'),[file,bin],{
    timeout:180000,env:{...process.env,ORESLANG_LLVMC:compiler,ORESLANG_LLVM_AOT_OPT_LEVEL:String(opt)}});
   if(native.status!==0)throw Error('native O'+opt+' AOT compilation failed: '+name+' '+native.stderr.slice(0,800));
   const bytes=readFileSync(bin);
   assert.equal(bytes.subarray(0,4).toString('hex'),'7f454c46','not an ELF guest');
   const guest=run(bin,[],{timeout:20000});
   assert.equal(guest.status,expected,'real native guest exit mismatch: '+name+' O'+opt);
   assert.equal(guest.stdout.trim(),expectedOutput,'real native stdout mismatch: '+name+' O'+opt);
   executions.push({opt,sha256:sha256(bytes),exit:guest.status,stdout:guest.stdout.trim()});
  }
  results.push({id:name,kind:'positive',blob,state:'NATIVE_GUEST_O0_O2_EXECUTED',
   guest_executed:true,executions});
  console.log('LLVM_ADT_GUARD_CASE='+name+' state=NATIVE_GUEST_O0_O2_EXECUTED');
 }
 const positives=results.filter(x=>x.guest_executed).length;
 const classified=results.filter(x=>x.semantic_negative_certified).length;
 const report={schema:'attolang.adt-guard-llvm-admission.v1',sourceHead,sourceTree,
   compilerSha256:sha256(readFileSync(compiler)),backendParityCertified:false,
   positivesNativeExecuted:positives,negativeSemanticProofs:classified,cases:results};
 const out=process.env.LLVM_ADT_GUARD_RECEIPT_PATH||join(runnerDir,'receipt.json');
 writeFileSync(out,JSON.stringify(report,null,2)+'\n');
 console.log('LLVM_ADT_GUARD_ADMISSION_DONE positives_native='+positives+
  ' negatives_semantic_certified='+classified+' parity=NOT_CERTIFIED receipt='+out);
}finally{if(!process.env.LLVM_ADT_GUARD_KEEP_TEMP)rmSync(runnerDir,{recursive:true,force:true});}
