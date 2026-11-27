import assert from 'node:assert/strict';
import {mkdtemp,writeFile,readFile,rm} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
const compiler=process.env.ATTOC||'./target/debug/attoc';
const work=await mkdtemp(join(tmpdir(),'atto-llvm-recursive-'));
const run=(exe,args,timeout)=>spawnSync(exe,args,{encoding:'utf8',timeout,maxBuffer:1<<20});
function ok(result,label){
  assert.equal(result.error,undefined,label+' infrastructure');
  assert.equal(result.signal,null,label+' signal');
  assert.equal(result.status,0,label+': '+result.stderr);
}
const worker=`import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const b=readFileSync(process.argv[1]),want=process.argv[2];
assert.ok(WebAssembly.validate(b));
const m=new WebAssembly.Module(b);
assert.deepEqual(WebAssembly.Module.imports(m),[]);
const i=new WebAssembly.Instance(m,{});
const isolated=process.argv[3]==='isolated';
if(want==='stack'){
  assert.throws(()=>i.exports.main(),WebAssembly.RuntimeError);
  if(!isolated)assert.equal(i.exports.panic_code.value,6);
}else{
  for(let n=0;n<3;n++){
    assert.equal(i.exports.main(),BigInt(want));
    if(!isolated)assert.equal(i.exports.panic_code.value,0);
  }
}
console.log('PASS '+want);`;
const count='fnc count(int n): int { return match n over on 0 -> 0, on _ -> count(n-1)+1 end; } ';
const mutual='fnc a(int n): int { return match n over on 0 -> 0, on _ -> b(n-1)+1 end; } fnc b(int n): int { return match n over on 0 -> 0, on _ -> a(n-1)+1 end; } ';
try{
  for(const [id,src,want] of [
    ['acyclic','fnc leaf(int n): int {return n+1;} pub fnc main(): int {return leaf(41);}','42'],
    ['direct',count+'pub fnc main(): int {return count(600);}','600'],
    ['boundary',count+'pub fnc main(): int {return count(1024);}','1024'],
    ['limit',count+'pub fnc main(): int {return count(1025);}','stack'],
    ['mutual',mutual+'pub fnc main(): int {return a(600);}','600'],
    ['mutual-limit',mutual+'pub fnc main(): int {return a(1025);}','stack'],
    ['no-base','fnc f(int n): int {return f(n-1);} pub fnc main(): int {return f(0);}','stack'],
  ]){
    for(const isolated of [false,true]){
      const input=join(work,id+'.atto'),out=join(work,id+(isolated?'-iso':'')+'.wasm');
      await writeFile(input,src);
      ok(run(compiler,[...(isolated?['--isolated-core']:[]),input,'-o',out],30000),id+' compile');
      const bytes=await readFile(out);
      assert.ok(WebAssembly.validate(bytes),id);
      const guest=run(process.execPath,['--input-type=module','-e',worker,out,want,isolated?'isolated':'normal'],10000);
      ok(guest,id+' guest');
      assert.equal(guest.stderr,'',id);
      assert.equal(guest.stdout,'PASS '+want+'\n',id);
      console.log('PASS',id,isolated?'isolated':'normal');
    }
  }
}finally{await rm(work,{recursive:true,force:true});}
