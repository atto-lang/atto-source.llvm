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
    ['named-fib-10','pub fnc main(): int {return 10 |> fnc fib(int v): int {^ match v over on 0 -> 0, on 1 -> 1, on _ -> fib(v-2)+fib(v-1) end };}','55'],
    ['named-shadow','fnc fib(int n): int {return 999;} pub fnc main(): int {return 10 |> fnc fib(int v): int {^ match v over on 0 -> 0, on 1 -> 1, on _ -> fib(v-2)+fib(v-1) end };}','55'],
    ['named-limit','pub fnc main(): int {return 1025 |> fnc f(int v): int {^ match v over on 0 -> 0, on _ -> f(v-1)+1 end };}','stack'],
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
  // Exact requested expensive expression: compile and validate, do not run.
  const exact=join(work,'named-fib55-exact.atto'),exactOut=join(work,'named-fib55-exact.wasm');
  await writeFile(exact,`pub fnc main(): int {
    return 55 |> fnc fib(int v): int {
        ^ match v over
            on 0 -> 0,
            on 1 -> 1,
            on _ -> fib(v - 2) + fib(v - 1)
        end
    };
}
`);
  ok(run(compiler,[exact,'-o',exactOut],30000),'exact named fib55 compile');
  assert.ok(WebAssembly.validate(await readFile(exactOut)));
  console.log('COMPILE ONLY exact fib55');
  for(const [id,source,diagnostic] of [
    ['capture','pub fnc main(): int {const int x=2; return 1 |> fnc f(int v): int {return v+x;};}',/E_UNSUPPORTED/],
    ['arity','pub fnc main(): int {return 1 |> fnc f(int v): int {return f(v,v);};}',/E_ARITY/],
    ['escape','pub fnc main(): int {val int x=1 |> fnc f(int v): int {return v;};return f(x);}',/E_UNBOUND/],
    ['routine','routine r(int n): int {return n;} pub fnc main(): int {return 1 |> fnc f(int v): int {return v;};}',/E_UNSUPPORTED/],
    ['collision','pub fnc main(): int {return 1 |> fnc f(int f): int {return f;};}',/E_DUPLICATE/],
  ]){
    const input=join(work,id+'.atto'),out=join(work,id+'.wasm');
    await writeFile(input,source);
    const result=run(compiler,[input,'-o',out],30000);
    assert.equal(result.error,undefined,id);
    assert.equal(result.signal,null,id);
    assert.equal(result.status,1,id);
    assert.match(result.stderr,diagnostic,id);
    await assert.rejects(readFile(out),{code:'ENOENT'});
    console.log('PASS rejection',id);
  }
  // Throw across 400 recursive edges while 599 caller edges remain live.
  // Restoring to zero would incorrectly admit the 500-edge probe; failing
  // to restore would incorrectly reject the 400-edge probe.
  const eh=count+`fnc walk(int n, int probe): int {
    if n == 0 { throw Error("unwind"); }
    if n < 0 { return walk(n+1, probe); }
    if n == 1 {
      return match trap walk(-400, probe) over
        on None -> count(probe),
        on Some(v) -> v
      end;
    }
    return walk(n-1, probe);
  } `;
  for(const [id,src,want] of [
    ['catch-restores-live-depth',eh+'pub fnc main(): int {return walk(600,400);}','400'],
    ['catch-does-not-reset-live-depth',eh+'pub fnc main(): int {return walk(600,500);}','stack'],
    ['catch-does-not-intercept-quota',count+'pub fnc main(): int {return match trap count(1025) over on None -> 99, on Some(v) -> v end;}','stack'],
  ]){
    const input=join(work,id+'.atto'),out=join(work,id+'.wasm');
    await writeFile(input,src);
    ok(run(compiler,[input,'-o',out],30000),id+' compile');
    const guest=run(process.execPath,['--input-type=module','-e',worker,out,want,'normal'],10000);
    ok(guest,id+' guest');
    assert.equal(guest.stderr,'',id);
    assert.equal(guest.stdout,'PASS '+want+'\n',id);
    console.log('PASS',id);
  }
}finally{await rm(work,{recursive:true,force:true});}
