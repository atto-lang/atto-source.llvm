import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtempSync,readFileSync,writeFileSync,rmSync,realpathSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
const compiler=realpathSync(process.env.ORESLANG_LLVMC??'build/oreslang-llvmc');
const clang=process.env.CLANG??'clang';
const work=mkdtempSync(join(tmpdir(),'atto-native-recur-'));
const run=(cmd,args)=>{
 const x=spawnSync(cmd,args,{encoding:'utf8',timeout:40000,maxBuffer:16*1024*1024});
 assert.ifError(x.error);assert.equal(x.signal,null,cmd+' terminated');return x;
};
const blob=b=>createHash('sha1').update('blob '+b.length+'\0').update(b).digest('hex');
function positive(id,source,expected){
 const p=join(work,id+'.atto');writeFileSync(p,source);
 const ir=run(compiler,['--target=host',p]);
 assert.equal(ir.status,0,id+': '+ir.stderr);
 assert.match(ir.stdout,/^define\s+i64\s+@__oreslang_guest_main\(\)/m);
 assert.match(ir.stdout,/recur\.state.*phi i64/);
 assert.match(ir.stdout,/recur\.result.*phi i64/);
 const wrapper=/^define\s+i32\s+@main\(\)([^\n{]*)\{/gm;
 assert.equal([...ir.stdout.matchAll(wrapper)].length,1);
 const ll=join(work,id+'.ll');
 writeFileSync(ll,ir.stdout.replace(wrapper,'define i32 @__atto_host_main_unused()$1{'));
 for(const opt of ['-O0','-O2']){
  const exe=join(work,id+opt);
  const link=run(clang,[opt,ll,join(work,'observer.c'),'-o',exe]);
  assert.equal(link.status,0,link.stderr);
  const guest=run(exe,[]);
  assert.equal(guest.status,0);
  assert.equal(guest.stdout,expected+'\n');
  assert.equal(guest.stderr,'');
  console.log('PASS real native guest '+id+' '+opt+' '+expected);
 }
}
function negative(id,expr){
 const p=join(work,id+'.atto');
 writeFileSync(p,'fnc f(int x): int { return x; } pub fnc main(): int { return '+expr+'; }');
 const result=run(compiler,['--target=host',p]);
 assert.ok(result.status!==0&&result.status!==null,id+' unexpectedly compiled');
 assert.equal(result.stdout,'',id+' emitted partial IR');
 assert.ok(result.stderr.trim().length>0,id+' missing diagnostic');
 console.log('PASS no-artifact denial '+id);
}
try{
 writeFileSync(join(work,'observer.c'),'#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\nextern int64_t __oreslang_guest_main(void);\nint main(void){printf("%" PRId64 "\\n",__oreslang_guest_main());return 0;}\n');
 positive('sum','pub fnc main(): int { return loop (val i:int=0,val sum:int=0) { if i>=10 {break sum;} recur(i+1,sum+i); }; }','45');
 positive('swap','pub fnc main(): int { return loop (val i=0,val a=2,val b=a+40) { if i eq 1 {break a;} else {recur(i+1,b,a);} }; }','42');
 positive('long','pub fnc main(): int {return loop (val i=0,val sum=0) {if i>=10000000 {break sum;} recur(i+1,sum+i);};}','49999995000000');
 positive('arg','fnc f(int n):int {return n;} pub fnc main():int {return 2+f(loop (val i=0) {if i eq 1 {break 40;} recur(i+1);});}','42');
 positive('ordered','pub fnc main():int {return loop (val i=0) {if i eq 1 {break 42;} if i eq 1 {break 7;} recur(i+1);};}','42');
 for(const [id,expr] of [
 ['arity','loop (val i=0) {if i eq 1 {break i;} recur(i+1,2);}'],
 ['forward','loop (val a=b,val b=42) {if b eq 42 {break a;} recur(a,b);}'],
 ['type','loop (val i=0) {if i eq 1 {break i;} recur(i eq 0);}'],
 ['guard','loop (val i=0) {if i {break i;} recur(i+1);}'],
 ['capture','loop (val i=f(0)) {if i eq 1 {break i;} recur(i+1);}'],
 ['nested','loop (val i=0) {if i eq 1 {break i;} recur(loop (val j=0) {if j eq 0 {break 1;} recur(j+1);});}']
 ])negative(id,expr);
 console.log('PASS 10 compiled native executions, 6 source denials');
}finally{rmSync(work,{recursive:true,force:true});}
