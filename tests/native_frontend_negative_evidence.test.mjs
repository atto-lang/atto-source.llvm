// Source-first LLVM compiler negative matrix. Not proof of full guest AOT.
import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {constants, openSync, fstatSync, readFileSync, closeSync, mkdtempSync,
  writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join, isAbsolute} from 'node:path';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('../',import.meta.url)));
const positive = ['valid-main-42', 'pub fnc main():int{return 42;}'];
const negative = Object.freeze([
  ['bool-as-int','pub fnc main():int{return true;}',
    'native function return requires int, got bool'],
  ['truthy-int','pub fnc main():int{if(1){return 42;}fi return 0;}',
    'native guarded return condition requires bool, got int'],
  ['bool-arithmetic','pub fnc main():int{return true+1;}',
    'arithmetic requires int, got bool'],
  ['comparison-type','pub fnc main():int{if(true eq 1){return 42;}fi return 0;}',
    'equality operands require the same scalar type'],
  ['not-integer','fnc bad():bool{return !1;}pub fnc main():int{return 0;}',
    "logical '!' requires bool, got int"],
  ['exclusive-alias','fnc pair(borrow mut bool a,borrow mut bool b):int{return 42;}pub fnc main():int{val flag:bool=true;return pair(rt mut borrow flag,rt mut borrow flag);}',
    'conflicting active native borrow'],
  ['shared-exclusive-alias','fnc pair(borrow bool a,borrow mut bool b):int{return 42;}pub fnc main():int{val flag:bool=true;return pair(rt borrow flag,rt mut borrow flag);}',
    'conflicting active native borrow'],
  ['readonly-mutable-loan','fnc read(borrow mut bool flag):int{return 42;}pub fnc main():int{const flag:bool=true;return read(rt mut borrow flag);}',
    'cannot mutably borrow immutable owner']
]);
const hex = (s,n) => typeof s==='string' && s.length===n &&
  /^[a-f0-9]+$/.test(s) && /[a-f1-9]/.test(s);
const sha256 = b => createHash('sha256').update(b).digest('hex');
export function assertNativeNegative(result, diagnostic) {
  if (!result || result.error || result.signal!==null || result.status!==1 ||
      result.stdout!=='' || typeof result.stderr!=='string' ||
      !/^oreslang-llvmc:\s+\S.+/m.test(result.stderr) ||
      !result.stderr.includes(diagnostic)) {
    throw Error('NOT_EXECUTED: expected native compiler rejection with no IR');
  }
  return true;
}
export function requireBuildIdentity(env) {
  if (!isAbsolute(env.ORESLANG_LLVMC||'') ||
      !hex(env.ATTO_EXPECTED_LLVM_HEAD,40) ||
      !hex(env.ATTO_EXPECTED_LLVM_COMPILER_SHA256,64)) {
    throw Error('NOT_EXECUTED: absolute native compiler and exact LLVM head required');
  }
  return env;
}
function readCompiler(path) {
  // Descriptor-based, bounded, symlink-resistant binary snapshot.
  if (!Number.isInteger(constants.O_NOFOLLOW) || !Number.isInteger(constants.O_NONBLOCK))
    throw Error('cannot safely open compiler on this host');
  const fd=openSync(path,constants.O_RDONLY|constants.O_NOFOLLOW|constants.O_NONBLOCK);
  try {
    const st=fstatSync(fd);
    assert.ok(st.isFile() && st.size>0 && st.size<=256*1024*1024,
      'compiler must be a bounded regular file');
    const bytes=readFileSync(fd);
    assert.equal(bytes.length,st.size,'compiler source changed during snapshot');
    return bytes;
  } finally {closeSync(fd);}
}
test('eight native LLVM negative source programs are distinct and diagnostic-specific (HOST ONLY)',()=>{
  assert.equal(negative.length,8);
  assert.equal(new Set(negative.map(x=>x[0])).size,negative.length);
  assert.equal(new Set(negative.map(x=>sha256(Buffer.from(x[1])))).size,negative.length);
  for(const [id,src,diag] of negative) {
    assert.match(id,/^[a-z][a-z0-9-]+$/);
    assert.ok(src.includes('main()') && src.length<4096);
    assert.ok(diag.length>14);
  }
});
test('rejects frontend success, crash, parser-only error, and leaked IR (HOST ONLY)',()=>{
  const diag=negative[0][2];
  const good={status:1,signal:null,stdout:'',stderr:'oreslang-llvmc: '+diag+'\n'};
  assert.equal(assertNativeNegative(good,diag),true);
  for(const patch of [{status:0},{status:2},{status:null},{signal:'SIGSEGV'},
    {error:Error('spawn')},{stdout:'define i64 @main() {}'},
    {stderr:'wasm bridge: '+diag},{stderr:'oreslang-llvmc: unrelated error'}]) {
    assert.throws(()=>assertNativeNegative({...good,...patch},diag),/NOT_EXECUTED/);
  }
});
test('missing or relative compiler identity cannot substitute for native execution (HOST ONLY)',()=>{
  assert.throws(()=>requireBuildIdentity({}),/NOT_EXECUTED/);
  assert.throws(()=>requireBuildIdentity({ORESLANG_LLVMC:'./compiler',
    ATTO_EXPECTED_LLVM_HEAD:'a'.repeat(40),
    ATTO_EXPECTED_LLVM_COMPILER_SHA256:'c'.repeat(64)}),/NOT_EXECUTED/);
  assert.throws(()=>requireBuildIdentity({ORESLANG_LLVMC:'/tmp/compiler',
    ATTO_EXPECTED_LLVM_HEAD:'A'.repeat(40),
    ATTO_EXPECTED_LLVM_COMPILER_SHA256:'c'.repeat(64)}),/NOT_EXECUTED/);
  assert.doesNotThrow(()=>requireBuildIdentity({ORESLANG_LLVMC:'/tmp/compiler',
    ATTO_EXPECTED_LLVM_HEAD:'a'.repeat(40),
    ATTO_EXPECTED_LLVM_COMPILER_SHA256:'c'.repeat(64)}));
});
const demand=process.env.ATTO_REQUIRE_NATIVE_LLVM_FRONTEND ?? '0';
if(!['0','1'].includes(demand))throw Error('ATTO_REQUIRE_NATIVE_LLVM_FRONTEND must be 0 or 1');
test('production compiler directly rejects eight invalid .atto sources before IR', {
  skip:demand!=='1',timeout:180000
},()=>{
  const env=requireBuildIdentity(process.env);
  const head=spawnSync('git',['rev-parse','HEAD'],{
    cwd:repo,encoding:'utf8',timeout:5000,maxBuffer:65536});
  assert.ifError(head.error);
  assert.equal(head.status,0);
  assert.equal(head.stdout.trim(),env.ATTO_EXPECTED_LLVM_HEAD,
    'compiler source is not exact CI checkout');
  const compilerBytes=readCompiler(env.ORESLANG_LLVMC);
  assert.equal(sha256(compilerBytes),env.ATTO_EXPECTED_LLVM_COMPILER_SHA256,
    'compiled executable bytes differ from expected digest');
  const dir=mkdtempSync(join(tmpdir(),'atto-llvm-negative-'));
  try {
    const compiler=join(dir,'oreslang-llvmc');
    writeFileSync(compiler,compilerBytes,{flag:'wx',mode:0o500});
    const run=(file)=>spawnSync(compiler,['--target=host',file],{
      cwd:repo,encoding:'utf8',timeout:30000,maxBuffer:4*1024*1024,
      env:{PATH:process.env.PATH||'/usr/bin:/bin',LANG:'C.UTF-8',LC_ALL:'C.UTF-8'}});
    const goodSource=join(dir,positive[0]+'.atto');
    writeFileSync(goodSource,positive[1],{flag:'wx',mode:0o400});
    const good=run(goodSource);
    assert.ifError(good.error);
    assert.equal(good.signal,null);
    assert.equal(good.status,0,good.stderr);
    assert.match(good.stdout,/\bdefine\b/,'positive code did not produce LLVM IR');
    assert.ok(good.stdout.length>100,'positive LLVM IR unexpectedly short');
    for(const [id,src,diagnostic] of negative) {
      const file=join(dir,id+'.atto');
      writeFileSync(file,src,{flag:'wx',mode:0o400});
      assertNativeNegative(run(file),diagnostic);
      assert.equal(readFileSync(file,'utf8'),src,
        id+' source changed after compiler rejection');
    }
    console.log('NATIVE_LLVM_NEGATIVE_8=EXECUTED; GUEST_AOT=NOT_CERTIFIED; COMPILER_SHA256='+sha256(compilerBytes));
  } finally {rmSync(dir,{recursive:true,force:true});}
});
