import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtempSync,readFileSync,writeFileSync,rmSync,realpathSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
const compiler=realpathSync(process.env.ORESLANG_LLVMC??'build/oreslang-llvmc');
const clang=process.env.CLANG??'clang';
const work=mkdtempSync(join(tmpdir(),'atto-native-value-loop-'));
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
 const exact=readFileSync(new URL('../contracts/value-loop-recur-v1/230-stateless-value-loop.atto',import.meta.url));
 assert.equal(blob(exact),'2c0359a6d2876958d6fb6831eb0dac3c6548c15b');
 positive('exact',exact,'42');
 positive('arithmetic','pub fnc main(): int { return loop { break 20+22; }; }','42');
 positive('lazy','pub fnc main(): int { return true ? loop { break 42; } : loop { break 9223372036854775807+1; }; }','42');
 for(const [id,expr] of [['bare','loop { break; }'],['multiple','loop { break 42; break 1; }'],['designator','loop { break f; }'],['computed','loop { break 42; }(1)']])negative(id,expr);
 console.log('PASS 6 compiled native executions, 4 source denials');
}finally{rmSync(work,{recursive:true,force:true});}
