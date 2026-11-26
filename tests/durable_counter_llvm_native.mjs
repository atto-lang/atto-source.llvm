import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync,writeFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
const cli=resolve(process.env.ATTO_LLVM_COMPILER||'build/oreslang-llvmc');
const dir=mkdtempSync(join(tmpdir(),'atto-llvm-durable-'));
const source='define durable actor Counter as state count: int = 0; receive(ActorMail<int> mail) -> void { self.count = self.count + mail.value; } end';
function run(bin,args,input){
  const r=spawnSync(bin,args,{cwd:dir,input,encoding:'utf8',timeout:60000,maxBuffer:1<<20});
  assert.equal(r.error,undefined);return r;
}
function emit(text,ok=true){
  writeFileSync(join(dir,'counter.atto'),text);
  const r=run(cli,['--durable-counter-actor-v1','counter.atto']);
  assert.equal(r.status,ok?0:2,r.stderr);
  if(!ok){assert.equal(r.stdout,'');assert.match(r.stderr,/E_DURABLE_COUNTER_ACTOR_ABI/);}
  return r.stdout;
}
try{
  const ir=emit(source);assert.equal(ir,emit(source));
  assert.equal(ir,emit(source.replaceAll('mail','event')));
  assert.notEqual(ir,emit(source.replaceAll('count','balance')));
  for(const bad of [
    source.replace('durable actor','actor'),source.replace('durable actor','persistent actor'),
    source.replace('= 0;','= 1;'),source.replace('state count','pub state count'),
    source.replace('self.count + mail.value','self.count - mail.value'),
    source.replace('mail.value','9'),source.replace('ActorMail<int>','ActorMail<bool>'),
    source.replace('self.count =','self.unknown ='),source.replace('receive(','pub receive('),
    source.replace('self.count = self.count + mail.value;','self.count = self.count + mail.value; self.count = 0;'),
    source+' pub fnc main()->int {return 0;}',source+'//\u0000',
    source.replaceAll('count','self'),source.replace('durable','"durable"'),
    source+' /* comment */',source.replace('= 0;','= 00;')
  ])emit(bad,false);
  writeFileSync(join(dir,'guest.ll'),ir);
  writeFileSync(join(dir,'driver.c'),`#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
extern int64_t atto_durable_transition_v1(int64_t,int64_t,int64_t);
extern uint64_t atto_durable_state_size_v1(void);
extern const unsigned char* atto_durable_state_v1(void);
int main(void){
  const unsigned char* p=atto_durable_state_v1();
  uint64_t n=atto_durable_state_size_v1();
  if(!p||n!=53)return 2;
  for(uint64_t i=0;i<n;i++)printf("%02x",(unsigned)p[i]);
  puts("");
  int64_t s,o,d;
  while(scanf("%" SCNd64 " %" SCNd64 " %" SCNd64,&s,&o,&d)==3)
    printf("%" PRId64 "\\n",atto_durable_transition_v1(s,o,d));
  return feof(stdin)?0:3;
}
`);
  const build=run(process.env.CLANG||'clang',['-O2','-Wno-override-module','guest.ll','driver.c','-o','guest']);
  assert.equal(build.status,0,build.stderr);
  const vectors=[],expected=[];
  function add(s,o,d){
    vectors.push([s,o,d]);
    expected.push(s<0n||s>1000000000n?-1n:o===2n?(d===0n?s:-1n):o!==1n||d<=0n||d>1000000000n||s+d>1000000000n?-1n:s+d);
  }
  for(let i=0n;i<100n;i++)add(i,1n,1n);
  for(const s of [-1n,0n,100n,999999999n,1000000000n,1000000001n])
    for(const o of [0n,1n,2n,3n])
      for(const d of [-1n,0n,1n,42n,1000000000n,1000000001n])add(s,o,d);
  const g=run(join(dir,'guest'),[],vectors.map(v=>v.join(' ')).join('\n')+'\n');
  assert.equal(g.status,0,g.stderr);assert.equal(g.stderr,'');
  const [hex,...values]=g.stdout.trim().split('\n');
  assert.equal(hex,'0107436f756e74657205636f756e7401244154544f53543031013782cc560597ec4601000100000000000000002b35b70ef92e387a');
  assert.equal(values.length,244);assert.deepEqual(values.map(BigInt),expected);
  console.log('LLVM_NATIVE_DURABLE_SCHEMA_AND_244_TRANSITIONS_PASS');
}finally{rmSync(dir,{recursive:true,force:true});}
