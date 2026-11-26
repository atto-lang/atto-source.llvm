import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const corpus = readFileSync(new URL('../contracts/atto-xo-checked-u64-debit-v1/cases.tsv', import.meta.url));
assert.equal(createHash('sha256').update(corpus).digest('hex'), '3a79dc1db8ee896fc2b39a4df136c102ebfa341f282b8d16785fb035b663ca58');
const rows = corpus.toString('utf8').trimEnd().split('\n').map(row => {
  const fields = row.split('|'); assert.equal(fields.length,9);
  return { id: fields[0], expected: fields[1], source: fields[8] };
});
assert.equal(rows.length,12);
assert.equal(new Set(rows.map(r=>r.id)).size,12);
const root = mkdtempSync(join(tmpdir(),'atto-u64-'));
const compiler = join(root,'compiler');
const clang = process.env.CLANG ?? 'clang';
const clangxx = process.env.CLANGXX ?? 'clang++';
const run = (cmd,args,input) => spawnSync(cmd,args,{input,encoding:'utf8',timeout:30000,maxBuffer:1024*1024,cwd:root});
function success(r) { assert.ifError(r.error); assert.equal(r.signal,null); assert.equal(r.status,0,r.stderr); return r.stdout; }
try {
  success(run(clangxx,['-std=c++20','-O2','-Wall','-Wextra','-Werror',fileURLToPath(new URL('../tools/atto_checked_u64.cpp',import.meta.url)),'-o',compiler]));
  writeFileSync(join(root,'observe.c'),'#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\nextern uint64_t atto_guest_main(void);\nint main(void){printf("%" PRIu64 "\\n",atto_guest_main());return 0;}\n');
} catch(e) { rmSync(root,{recursive:true,force:true}); throw e; }
function lower(source) { return success(run(compiler,[],source)); }
async function observe(source,expected,opt,wasm) {
  const ir = lower(source); writeFileSync(join(root,'guest.ll'),ir);
  if (wasm) {
    success(run(clang,['--target=wasm32-unknown-unknown','-Wno-override-module',opt,'-nostdlib','-Wl,--no-entry','-Wl,--export=atto_guest_main','guest.ll','-o','guest.wasm']));
    const {instance}=await WebAssembly.instantiate(readFileSync(join(root,'guest.wasm')));
    const main=instance.exports.atto_guest_main;
    if(expected==='overflow') assert.throws(()=>main(),WebAssembly.RuntimeError);
    else assert.equal(BigInt.asUintN(64,main()).toString(),expected);
  } else {
    success(run(clang,['-Wno-override-module',opt,'guest.ll','observe.c','-o','guest']));
    const result=run(join(root,'guest'),[]);
    if(expected==='overflow') {
      assert.ifError(result.error);
      assert.ok(['SIGILL','SIGTRAP'].includes(result.signal),JSON.stringify({status:result.status,signal:result.signal,stderr:result.stderr}));
      assert.equal(result.stdout,'');
    } else assert.equal(success(result),expected+'\n');
  }
}
try {
  for(const row of rows) for(const opt of ['-O0','-O2']) for(const wasm of [false,true]) {
    await test(`${row.id} ${opt} ${wasm?'wasm32':'native'}`,()=>observe(row.source,row.expected,opt,wasm));
  }
  await test('arbitrary admitted source, precedence, aliases and forward calls',async()=>{
    const source='pub fnc main(): uint { return later((4+5)*6)-3-2; } fnc later(u64 n): u64 { return n+1; }';
    for(const opt of ['-O0','-O2']) for(const wasm of [false,true]) await observe(source,'50',opt,wasm);
  });
  await test('explicit target selection and no implicit unsupported target',()=>{
    const source='pub fnc main():u64{return 42;}';
    assert.equal(success(run(compiler,['--emit=llvm'],source)),lower(source));
    assert.match(success(run(compiler,['--emit=cpp'],source)),/std::uint64_t/);
    for(const args of [['--emit=unknown'],['--emit=cpp','extra']]) {
      const r=run(compiler,args,source);assert.equal(r.status,2);assert.equal(r.stdout,'');assert.match(r.stderr,/unsupported target/);
    }
  });
  await test('refuse invalid source before emitting IR',()=>{
    const guest=body=>'pub fnc main(): u64 {return '+body+';}';
    for(const source of [guest('18446744073709551616'),guest('-1'),guest('1/2'),guest('x'),guest('main()'),guest('f(1)')+'fnc f():u64{return 1;}',guest('1')+'fnc f(int n):u64{return 1;}',guest('1')+'fnc f(u64 n,u64 n):u64{return n;}',guest('1')+guest('2'),guest('1')+'fnc f():u64{return g();} fnc g():u64{return f();}',guest('1'+'+1'.repeat(100)),guest('('.repeat(40)+'1'+')'.repeat(40)),guest('01'),guest('1')+'\u202e',guest('1')+' // comment',guest('1')+' '.repeat(17000)]) {
      const r=run(compiler,[],source); assert.ifError(r.error); assert.equal(r.status,2); assert.equal(r.stdout,''); assert.match(r.stderr,/E_U64_PROFILE/);
    }
    let s=guest('f15()')+'fnc f0():u64{return 1;}';
    for(let i=1;i<=15;i++) s+=`fnc f${i}():u64{return f${i-1}()+f${i-1}();}`;
    const r=run(compiler,[],s); assert.equal(r.status,2); assert.equal(r.stdout,''); assert.match(r.stderr,/work budget/);
  });
} finally { rmSync(root,{recursive:true,force:true}); }
