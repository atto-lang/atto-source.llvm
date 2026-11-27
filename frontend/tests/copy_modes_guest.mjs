import assert from 'node:assert/strict';
import {mkdtempSync,writeFileSync,readFileSync,existsSync,rmSync} from 'node:fs';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
const compiler=resolve(process.env.ATTOC??'target/debug/attoc');
const flat='define class Box as\n pub let int n = 20;\nend\n';
const nested='define class Leaf as pub val int n = 20; end\ndefine class Root as pub let Leaf child = new Leaf(); pub let int extra = 1; end\n';
const mutableNested='define class Scores as pub let int n = 20; end\ndefine class Person as pub let mut Scores scores = new Scores(); pub let int age = 1; end\n';
const fixedNested=mutableNested.replace('pub let mut Scores','pub val Scores');
const deepTree='define class Scores as pub let int n = 20; end\ndefine class Profile as pub let mut Scores scores = new Scores(); end\ndefine class Person as pub let mut Profile profile = new Profile(); pub let int age = 1; end\n';
const main=s=>'pub fnc main(): int { '+s+' }';
const cases=[
 ['exclusive-call-detach',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; }\n'+main('let mut Person a = new Person(); let mut Person b = copy! a; b.scores = new Scores(); adjust(mut b.scores); return a.scores.n+b.scores.n;'),42],
 ['exclusive-call-release',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; }\n'+main('let mut Person a = new Person(); let int n = 0; block { let mut Person b = copy! a; n = b.scores.n; } adjust(mut a.scores); return n+a.scores.n;'),42],
 ['exclusive-call-shared-source',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; }\n'+main('let mut Person a = new Person(); let mut Person b = copy! a; adjust(mut a.scores); return b.scores.n;'),'E_COPY_READONLY'],
 ['exclusive-call-shared-copy',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; }\n'+main('let mut Person a = new Person(); let mut Person b = copy! a; adjust(mut b.scores); return a.scores.n;'),'E_COPY_READONLY'],
 ['exclusive-call-missing-mut',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; }\n'+main('let mut Person a = new Person(); adjust(a.scores); return a.scores.n;'),'E_BORROW'],
 ['exclusive-call-return-escape',mutableNested+'fnc escape(mut Scores s): Scores { return s; }\n'+main('return 42;'),'E_COPY_UNPROVEN'],
 ['exclusive-call-nested-effect',mutableNested+'fnc adjust(mut Scores s): void { s.n = 22; stdio.println(s.n); }\n'+main('return 42;'),'E_COPY_UNPROVEN'],
 ['exclusive-call-unproved-callee',mutableNested+'fnc read(Scores s): int { return s.n; }\n'+main('let mut Person a = new Person(); return read(mut a.scores);'),'E_BORROW'],
 ['tree-detach',deepTree+main('let mut Person a = new Person(); let mut Person b = copy! a; let mut Person c = copy! b; b.profile = new Profile(); b.profile.scores.n = 22; return c.profile.scores.n + b.profile.scores.n;'),42],
 ['tree-deep',deepTree+main('let mut Person a = new Person(); a.profile.scores.n = 10; let mut Person b = copy! a; let mut Person c = copy!! b; c.profile.scores.n = 22; return a.profile.scores.n+b.profile.scores.n+c.profile.scores.n;'),42],
 ['tree-release',deepTree+main('let mut Person a = new Person(); let int before = 0; block { let mut Person b = copy! a; before = b.profile.scores.n; } a.profile.scores.n = 22; return before+a.profile.scores.n;'),42],
 ['tree-shared-write',deepTree+main('let mut Person a = new Person(); let mut Person b = copy! a; a.profile.scores.n = 22; return b.profile.scores.n;'),'E_COPY_READONLY'],
 ['tree-inner-replace',deepTree+main('let mut Person a = new Person(); a.profile.scores = new Scores(); return a.profile.scores.n;'),'E_COPY_UNPROVEN'],
 ['tree-shared-inner-replace',deepTree+main('let mut Person a = new Person(); let mut Person b = copy! a; b.profile.scores = new Scores(); return a.profile.scores.n;'),'E_COPY_READONLY'],
 ['tree-cycle','define class LoopNode as pub let mut LoopNode next = new LoopNode(); end\n'+main('return 42;'),'E_UNSUPPORTED_TYPE'],
 ['tree-depth',deepTree+'define class Outer as pub let mut Person person = new Person(); end\n'+main('return 42;'),'E_COPY_UNPROVEN'],
 ['mutable-detach',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; b.scores = new Scores(); b.scores.n = 22; return a.scores.n + b.scores.n;'),42],
 ['mutable-deep',mutableNested+main('let mut Person a = new Person(); a.scores.n = 10; let mut Person b = copy! a; let mut Person c = copy!! b; c.scores.n = 22; return a.scores.n + b.scores.n + c.scores.n;'),42],
 ['lexical-release',mutableNested+main('let mut Person a = new Person(); let int before = 0; block { let mut Person b = copy! a; block { let mut Person c = copy! b; before = c.scores.n; } } a.scores.n = 22; return before + a.scores.n;'),42],
 ['detach-source-recovery',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; b.scores = new Scores(); a.scores.n = 21; b.scores.n = 21; return a.scores.n+b.scores.n;'),42],
 ['fixed-nested-slot',fixedNested+main('let mut Person a = new Person(); let mut Person b = copy! a; let mut Person c = copy!! b; a.age = 2; return a.scores.n+c.scores.n+a.age;'),42],
 ['fixed-nested-replace',fixedNested+main('let mut Person a = new Person(); let mut Person b = copy! a; b.scores = new Scores(); return b.scores.n;'),'E_COPY_READONLY_SLOT'],
 ['source-shared-write',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; a.scores.n = 22; return b.scores.n;'),'E_COPY_READONLY'],
 ['copy-shared-write',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; b.scores.n = 22; return a.scores.n;'),'E_COPY_READONLY'],
 ['live-sibling',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; block { let mut Person c = copy! b; } a.scores.n = 22; return b.scores.n;'),'E_COPY_READONLY'],
 ['root-transfer',mutableNested+main('let mut Person a = new Person(); block { let mut Person b = copy! a; let escaped = b; } a.scores.n = 22; return a.scores.n;'),'E_COPY_UNPROVEN'],
 ['root-rebind',mutableNested+main('let mut Person a = new Person(); let mut Person b = copy! a; b = new Person(); return a.scores.n;'),'E_COPY_UNPROVEN'],
 ['root-boundary',mutableNested+'fnc read(Person x): int { return x.scores.n; }\n'+main('let mut Person a = new Person(); return read(a);'),'E_COPY_UNPROVEN'],
 ['root-temporary',mutableNested+main('let mut Person a = new Person(); copy! a; return a.scores.n;'),'E_COPY_UNPROVEN'],
 ['nested-immutable',nested+main('let Root a = new Root(); let Root b = copy! a; let Root c = copy!! b; a.extra = 2; b.extra = 3; c.extra = 4; b.child = new Leaf(); return a.child.n + c.child.n + a.extra;'),42],
 ['nested-escape',nested+main('let Root a = new Root(); let Root b = copy! a; let Leaf x = b.child; return x.n;'),'E_BORROW'],
 ['nested-write',nested+main('let Root a = new Root(); let Root b = copy! a; b.child.n = 0; return a.child.n;'),'E_OWNERSHIP'],
 ['mutable-leaf',nested.replace('pub val int n','pub let int n')+main('let Root a = new Root(); return a.child.n;'),'E_UNSUPPORTED_TYPE'],
 ['nested-method-leaf',nested.replace('n = 20; end','n = 20; read(): int { return self.n; } end')+main('let Root a = new Root(); return a.child.n;'),'E_UNSUPPORTED_TYPE'],
 ['nested-rebind-alias',nested+main('let Root a = new Root(); let Root b = copy! a; let Leaf x = new Leaf(); b.child = x; return a.child.n;'),'E_COPY_UNPROVEN'],
 ['scalar-modes',main('let int a = 20; let int b = copy! a; let int c = copy!! a; a = 2; return a+b+c;'),42],
 ['nested-negation',main('let bool a = false; let bool b = copy! !a; let bool c = copy!! !a; return (b && c) ? 42 : 0;'),42],
 ['precedence',main('return copy! 20 + copy!! 22;'),42],
 ['flat-shallow',flat+main('let Box a = new Box(); let Box b = copy! a; a.n = 11; b.n = 31; return a.n+b.n;'),42],
 ['flat-deep',flat+main('let Box a = new Box(); let Box b = copy!! a; a.n = 12; b.n = 30; return a.n+b.n;'),42],
 ['flat-third',flat+main('let Box a = new Box(); let Box b = copy! a; let Box c = copy! b; a.n = 10; b.n = 12; return a.n+b.n+c.n;'),42],
 ['moved-source',flat+main('let Box a = new Box(); let Box b = a; let Box c = copy! a; return b.n+c.n;'),'E_MOVED'],
 ['temporary-class-call',flat+'\nfnc read(Box x): int { return x.n; }\n'+main('let Box a = new Box(); return read(copy! a);'),'E_COPY_UNPROVEN'],
 ['actor-ref-denial','define actor A receives int as\n receive(ActorMail<int> mail): void { return; }\nend\n'+main('let a = spawn A(); let b = copy! a; return 42;'),'E_COPY_UNPROVEN'],
 ['fixed-slot',flat.replace('pub let','pub val')+main('let Box a = new Box(); let Box b = copy! a; b.n = 5; return 42;'),'E_IMMUTABLE'],
 ['immutable-root',flat+main('let Box a = new Box(); val Box b = copy! a; b.n = 5; return 42;'),'E_IMMUTABLE'],
 ['private-slot',flat.replace('pub let','let')+main('let Box a = new Box(); let Box b = copy! a; b.n = 5; return 42;'),'E_PRIVATE'],
 ['rebound-root',flat+main('let Box a = new Box(); let Box b = copy! a; b = new Box(); b.n = 5; return 42;'),'E_OWNERSHIP'],
 ['rhs-moves-root',flat+'\nfnc take_value(Box x): int { return x.n; }\n'+main('let Box a = new Box(); let Box b = copy! a; b.n = take_value(b); return 42;'),'E_OWNERSHIP'],
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
   const imports=WebAssembly.Module.imports(mod); const allowed=new Set(['print_i64','print_bool','print_f64','print_str','print_newline','actor_spawn','actor_send']); for(const i of imports){assert.equal(i.module,'atto');assert.equal(i.kind,'function');assert.ok(allowed.has(i.name));} const instance=new WebAssembly.Instance(mod,{atto:Object.fromEntries(imports.map(i=>[i.name,()=>{throw Error('unexpected host effect')}]))});
   assert.equal(instance.exports.main(),BigInt(expected),id);assert.equal(instance.exports.panic_code.value,0,id);
  } else {
   assert.equal(r.status,1,id+': '+r.stderr);assert.ok(r.stderr.includes(expected),id+': '+r.stderr);
   assert.equal(existsSync(output),false,id+': emitted rejected artifact');
  }
  console.log('PASS',id,expected);
 }
}finally{rmSync(dir,{recursive:true,force:true});}
