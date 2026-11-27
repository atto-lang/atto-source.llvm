import assert from 'node:assert/strict';
import {mkdtempSync,writeFileSync,readFileSync,existsSync,rmSync} from 'node:fs';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
const compiler=resolve(process.env.ATTOC??'target/debug/attoc');
const flat='define class Box as\n pub let int n = 20;\nend\n';
const main=s=>'pub fnc main(): int { '+s+' }';
const cases=[
 ['scalar-modes',main('let int a = 20; let int b = copy! a; let int c = copy!! a; a = 2; return a+b+c;'),42],
 ['nested-negation',main('let bool a = false; let bool b = copy! !a; let bool c = copy!! !a; if b && c { return 42; } else { return 0; }'),42],
 ['precedence',main('return copy! 20 + copy!! 22;'),42],
 ['flat-shallow',flat+main('let Box a = new Box(); let Box b = copy! a; a.n = 11; b.n = 31; return a.n+b.n;'),42],
 ['flat-deep',flat+main('let Box a = new Box(); let Box b = copy!! a; a.n = 12; b.n = 30; return a.n+b.n;'),42],
 ['flat-third',flat+main('let Box a = new Box(); let Box b = copy! a; let Box c = copy! b; a.n = 10; b.n = 12; return a.n+b.n+c.n;'),42],
 ['moved-source',flat+main('let Box a = new Box(); let Box b = a; let Box c = copy! a; return b.n+c.n;'),'E_MOVED'],
 ['temporary-class-call',flat+'\nfnc read(Box x): int { return x.n; }\n'+main('let Box a = new Box(); return read(copy! a);'),'E_COPY_UNPROVEN'],
 ['actor-ref-denial','define actor A as\nend\n'+main('let a = spawn A(); let b = copy! a; return 42;'),'E_COPY_UNPROVEN'],
 ['bad-bang',main('return copy!!! 42;'),'E_TYPE'],
];
const dir=mkdtempSync(join(tmpdir(),'atto-copy-modes-'));
try{
 for(const [id,source,expected] of cases){
  const input=join(dir,id+'.atto'), output=join(dir,id+'.wasm');
  writeFileSync(input,source);
  const r=spawnSync(compiler,[input,'-o',output],{encoding:'utf8',timeout:30000,maxBuffer:1048576});
  assert.equal(r.error,undefined,id); assert.equal(r.signal,null,id);
  if(typeof expected==='number'){
   assert.equal(r.status,0,id+': '+r.stderr);
   const bytes=readFileSync(output);assert.ok(WebAssembly.validate(bytes),id);
   const mod=new WebAssembly.Module(bytes);
   const instance=new WebAssembly.Instance(mod,{atto:Object.fromEntries(WebAssembly.Module.imports(mod).map(i=>[i.name,()=>{throw Error('unexpected host effect')}]))});
   assert.equal(instance.exports.main(),BigInt(expected),id);assert.equal(instance.exports.panic_code.value,0,id);
  } else {
   assert.equal(r.status,1,id+': '+r.stderr);assert.ok(r.stderr.includes(expected),id+': '+r.stderr);
   assert.equal(existsSync(output),false,id+': emitted rejected artifact');
  }
  console.log('PASS',id,expected);
 }
}finally{rmSync(dir,{recursive:true,force:true});}
