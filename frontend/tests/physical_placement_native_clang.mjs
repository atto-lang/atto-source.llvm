import assert from 'node:assert/strict';
import {mkdtemp,readFile,writeFile,rm,stat} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
const compiler=resolve(process.env.ATTOC??'target/release/attoc');
const root=await mkdtemp(join(tmpdir(),'atto-native-placement-'));
const run=(exe,args)=>{
  const r=spawnSync(exe,args,{encoding:'utf8',timeout:30000});
  assert.ifError(r.error); assert.equal(r.signal,null,`unexpected signal: ${exe}`);
  assert.ok(Number.isInteger(r.status),`missing exit status: ${exe}`);
  return r;
};
let serial=0;
async function compile(source){
  const name=join(root,'case-'+serial++),input=name+'.atto',c=name+'.c';
  await writeFile(input,source);
  const r=run(compiler,['--emit=physical-placement-c',input,'-o',c]);
  return {r,c};
}
const fixtures=["define struct Packet { i8 tag; int count; i16 tail; } with { constructor(i8 tag, int count, i16 tail) { self.tag = tag; self.count = count; self.tail = tail; } } pub fnc main(): int { let Storage<Packet> s = alloc! Packet; let mut RawPointer<Packet> p = RawPointer.empty<Packet>(); new Packet(-3, 40, 5) at s; ref! p to s; val a = (deref! p).tag; val b = (deref! p).count; val c = (deref! p).tail; unref! p; destroy! s; free! s; return a + b + c; }","define struct Payload { int count; i8 flag; } with { constructor(int count, i8 flag) { self.count = count; self.flag = flag; } } define struct Packet { i8 tag; Payload pair; i16 tail; } with { constructor(i8 tag, Payload pair, i16 tail) { self.tag = tag; self.pair = pair; self.tail = tail; } } pub fnc main(): int { let Storage<Packet> s = alloc! Packet; let mut RawPointer<Packet> p = RawPointer.empty<Packet>(); new Packet(-3, Payload(40, -2), 7) at s; ref! p to s; val a = (deref! p).tag; val b = (deref! p).pair.count; val c = (deref! p).pair.flag; val d = (deref! p).tail; unref! p; destroy! s; free! s; return a + b + c + d; }"];
try{
  for(const [source,size,offsets] of [[fixtures[0],24,[0,8,16]],[fixtures[1],32,[0,8,16,24]]]){
    const {r,c}=await compile(source);assert.equal(r.status,0,r.stderr);
    const generated=await readFile(c,'utf8');
    assert.match(generated,new RegExp('== '+size+', "size"'));
    for(const offset of offsets)assert.match(generated,new RegExp('== '+offset+', "offset"'));
    const host=join(root,'host.c');
    await writeFile(host,'#include <stdint.h>\n#include <assert.h>\nextern int32_t atto_placement_entry(int64_t*);\nint main(void){for(int i=0;i<100;i++){int64_t out=-1;assert(atto_placement_entry(&out)==0);assert(out==42);}assert(atto_placement_entry(0)==2);return 0;}\n');
    for(const opt of ['-O0','-O2']){
      const bin=c+'.'+opt.slice(1),ir=bin+'.ll';
      const emitted=run('clang',['-std=c11','-Wall','-Wextra','-Werror',opt,'-S','-emit-llvm',c,'-o',ir]);assert.equal(emitted.status,0,emitted.stderr);
      const llvm=await readFile(ir,'utf8');assert.match(llvm,/store volatile i8/);assert.match(llvm,/load volatile i64/);
      const link=run('clang',['-std=c11','-Wall','-Wextra','-Werror',opt,c,host,'-o',bin]);assert.equal(link.status,0,link.stderr);
      const guest=run(bin,[]);assert.equal(guest.status,0,guest.stderr);
      console.log('LLVM_PHYSICAL_PLACEMENT_NATIVE_PASS',JSON.stringify({size,opt,repetitions:100}));
    }
  }
  const pair=(lhs,rhs)=>`define struct CoordPair { int left; int right; } with { constructor(int left, int right) { self.left = left; self.right = right; } } pub fnc main(): int { let Storage<CoordPair> s = alloc! CoordPair; let mut RawPointer<CoordPair> p = RawPointer.empty<CoordPair>(); new CoordPair(${lhs}, ${rhs}) at s; ref! p to s; val a = (deref! p).left; val b = (deref! p).right; unref! p; destroy! s; free! s; return a - b + b; }`;
  for(const [lhs,rhs,expected] of [['42','-3','42'],['-9223372036854775808','-9223372036854775808','INT64_MIN'],['9223372036854775807','9223372036854775807','INT64_MAX']]){
    const {r,c}=await compile(pair(lhs,rhs));assert.equal(r.status,0,r.stderr);
    const generated=await readFile(c,'utf8');assert.match(generated,/__builtin_sub_overflow/);
    const host=join(root,'sub-host.c');
    await writeFile(host,`#include <stdint.h>\n#include <assert.h>\nextern int32_t atto_placement_entry(int64_t*);\nint main(void){for(int i=0;i<100;i++){int64_t out=777;assert(atto_placement_entry(&out)==0);assert(out==${expected});}assert(atto_placement_entry(0)==2);return 0;}\n`);
    for(const opt of ['-O0','-O2']){
      const bin=c+'.'+opt.slice(1),ir=bin+'.ll';
      const emitted=run('clang',['-std=c11','-Wall','-Wextra','-Werror',opt,'-S','-emit-llvm',c,'-o',ir]);assert.equal(emitted.status,0,emitted.stderr);
      const llvm=await readFile(ir,'utf8');assert.match(llvm,/llvm.ssub.with.overflow.i64/);
      const link=run('clang',['-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',opt,c,host,'-o',bin]);assert.equal(link.status,0,link.stderr);
      const guest=run(bin,[]);assert.equal(guest.status,0,guest.stderr);
      console.log('LLVM_CHECKED_SUB_PLACEMENT_NATIVE_PASS',JSON.stringify({lhs,rhs,opt,repetitions:100}));
    }
  }
  for(const [lhs,rhs] of [['-9223372036854775808','1'],['9223372036854775807','-1'],['0','-9223372036854775808'],['9223372036854775808','0'],['-9223372036854775809','0']]){
    const {r,c}=await compile(pair(lhs,rhs));assert.equal(r.status,1,r.stderr);assert.equal(r.stdout,'');await assert.rejects(stat(c),{code:'ENOENT'});
  }
  console.log('LLVM_CHECKED_SUB_PLACEMENT_NEGATIVE_PASS count=5');
  const arraySource="define struct Packet { i8 tag; Array<i16,3> cells; int tail; } with { constructor(i8 tag, Array<i16,3> cells, int tail) { self.tag = tag; self.cells = cells; self.tail = tail; } } pub fnc main(): int { let Storage<Packet> s = alloc! Packet; let mut RawPointer<Packet> p = RawPointer.empty<Packet>(); new Packet(-1, Array.of(10,20,11), 2) at s; ref! p to s; val a = (deref! p).tag; val b = (deref! p).cells[0]; val c = (deref! p).cells[1]; val d = (deref! p).cells[2]; val e = (deref! p).tail; unref! p; destroy! s; free! s; return a + b + c + d + e; }";
  const {r:arrayResult,c:arrayC}=await compile(arraySource);assert.equal(arrayResult.status,0,arrayResult.stderr);
  const arrayGenerated=await readFile(arrayC,'utf8');assert.match(arrayGenerated,/int16_t f1\[3\]/);
  for(const offset of [0,2,4,6,8])assert.match(arrayGenerated,new RegExp('== '+offset+', "offset"'));
  const arrayHost=join(root,'array-host.c');
  await writeFile(arrayHost,'#include <stdint.h>\n#include <assert.h>\nextern int32_t atto_placement_entry(int64_t*);\nint main(void){for(int i=0;i<100;i++){int64_t out=-1;assert(atto_placement_entry(&out)==0 && out==42);}return 0;}\n');
  for(const opt of ['-O0','-O2']){
    const bin=arrayC+'.'+opt.slice(1),ir=bin+'.ll';
    const emitted=run('clang',['-std=c11','-Wall','-Wextra','-Werror',opt,'-S','-emit-llvm',arrayC,'-o',ir]);assert.equal(emitted.status,0,emitted.stderr);
    const llvm=await readFile(ir,'utf8');assert.match(llvm,/load volatile i16/);assert.match(llvm,/store volatile i16/);
    const link=run('clang',['-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',opt,arrayC,arrayHost,'-o',bin]);assert.equal(link.status,0,link.stderr);
    assert.equal(run(bin,[]).status,0);console.log('LLVM_FIXED_ARRAY_PLACEMENT_NATIVE_PASS',JSON.stringify({opt,repetitions:100}));
  }
  for(const [from,to] of [["Array<i16,3> cells;","Array<i16,0> cells;"],["Array<i16,3> cells;","Array<i16,9> cells;"],["Array<i16,3> cells,","Array<i16,2> cells,"],["Array.of(10,20,11)","Array.of(10,20)"],["Array.of(10,20,11)","Array.of(10,20,32768)"],[".cells[2]",".cells[3]"],[".cells[2]",".cells[-1]"],[".cells[2]",".cells[tail]"],[".cells[2]",".cells"],[".cells[2]",".cells[2][0]"],[".tag;",".tag[0];"],["Array.of(10,20,11)","Array.of(10,20,11,12)"]]){
    const bad=arraySource.replace(from,to);assert.notEqual(bad,arraySource);
    const {r,c}=await compile(bad);assert.equal(r.status,1,bad);assert.equal(r.stdout,'');await assert.rejects(stat(c),{code:'ENOENT'});
  }
  console.log('LLVM_FIXED_ARRAY_PLACEMENT_NEGATIVE_PASS count=12');
  const nested=fixtures[1],flat=fixtures[0];
  for(const [source,from,to] of [
    [flat,'-3, 40, 5','128, 40, 5'],[flat,'ref! p','ref!p'],[flat,'ref! p','ref ! p'],
    [flat,'unref! p;','destroy! s;'],[flat,'unref! p;','p = null;'],
    [flat,'destroy! s;',''],[flat,'free! s;','free! s; free! s;'],
    [flat,'Storage<Packet>','Storage<Other>'],[flat,'self.tail = tail;','self.tag = tail;'],
    [nested,'Payload pair;','Packet pair;'],[nested,'Payload pair;','Missing pair;'],
    [nested,'Payload(40, -2)','Other(40, -2)'],[nested,'Payload(40, -2)','Payload(40, 128)'],
    [nested,'.pair.count;','.pair;'],[nested,'.pair.flag;','.pair.missing;'],
    [nested,'.pair.count;','.tag.count;'],[nested,'self.pair = pair;','self.pair = tail;'],
    [nested,'Payload pair, i16 tail','int pair, i16 tail'],[nested,'Payload(40, -2)','Payload(40)'],
    [nested,'return a + b + c + d;','return a + missing;'],
  ]){
    const bad=source.replace(from,to);assert.notEqual(bad,source,from);
    const {r,c}=await compile(bad);assert.equal(r.status,1,bad);
    assert.equal(r.stdout,'');await assert.rejects(stat(c),{code:'ENOENT'});
  }
  console.log('LLVM_PHYSICAL_PLACEMENT_NEGATIVE_PASS count=20');
}finally{await rm(root,{recursive:true,force:true})}
