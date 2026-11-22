// Source-level shape checks and signed-i64 BigInt algorithm oracle.
// NOT compiled AttoLang guest, parser, borrow-checker, or CI evidence.
import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
const read=path=>readFileSync(new URL(path,import.meta.url),'utf8');
const MIN=-(1n<<63n), MAX=(1n<<63n)-1n;
const MASK=(1n<<64n)-1n, wrap=n=>BigInt.asIntN(64,n);
const range=read('../core/range.atto');
const cell=read('../core/cell.atto');
const num=read('../core/num.atto');
const contracts=read('../core/contracts.atto');
const manifest=JSON.parse(read('../contracts/core-four-v1.json'));
function add(a,b) {
  if(b>0n && a>MAX-b) return MIN+(a-(MAX-b)-1n);
  if(b<0n && a<MIN-b) return MAX+b-(MIN-a)+1n;
  return a+b;
}
function sub(a,b) {
  if(b<0n && a>MAX+b) return MIN+a-(MAX+b)-1n;
  if(b>0n && a<MIN+b) return MAX+a-(MIN+b)+1n;
  return a-b;
}
function mul(a,b) {
  if(b===MIN) return (a%2n)!==0n?MIN:0n;
  let factor=b, negative=false;
  if(factor<0n) {factor=-factor;negative=true;}
  let product=0n,term=a;
  for(let i=0;i<63;i++) {
    if(factor%2n!==0n) product=add(product,term);
    factor=factor/2n;term=add(term,term);
  }
  if(negative) return sub(0n,product);
  return product;
}
function canMul(a,b) {
  if(a===0n||b===0n) return true;
  if(a>0n) return b>0n ? a<=MAX/b : b>=MIN/a;
  return b>0n ? a>=MIN/b : a>=MAX/b;
}
test('four Atto module sources exist, without host language fallback',()=>{
  assert.equal(manifest.schema,'attolang.core-four.v1');
  assert.match(manifest.status,/NOT_GUEST_VERIFIED/);
  for(const [name,source] of Object.entries({range,cell,num,contracts})) {
    assert.match(source,new RegExp('define module '+name+' as'));
    assert.doesNotMatch(source,/\b(?:import java|extern "C"|unsafe_host|native_binding)\b/i);
  }
  assert.match(read('../tests/fixtures/core-four-v1/smoke.atto'),/return 42;/);
  assert.match(read('../tests/fixtures/core-four-v1/contract-failure.atto'),/contracts\.requires\(false,/);
});
test('borrow admission saturates and never claims compiler-enforced Cell',()=>{
  assert.match(cell,/state >= 0 && state < 9223372036854775807/);
  assert.match(cell,/state == 0/);
  assert.match(cell,/state != -1/);
  assert.match(cell,/Result<int, String>/);
  assert.match(manifest.cell.scope,/POLICY_ONLY/);
});
test('contract failure and range endpoint guards have required source shape',()=>{
  for(const f of ['requires','ensures','invariant'])
    assert.match(contracts,new RegExp('pub fnc '+f+'\\(bool condition, String message\\): void'));
  assert.match(contracts,/rt fail\(message\)/);
  assert.match(range,/current > 9223372036854775807 - step/);
  assert.match(range,/current < -9223372036854775808 \+ step/);
  assert.match(range,/start <= value && value < end/);
  assert.match(range,/start <= value && value <= end/);
  assert.match(num,/pub fnc checked_add/);
  assert.match(num,/pub fnc checked_div/);
  assert.match(num,/pub fnc wrapping_mul/);
});
test('source-modeled i64 operations agree with independent exact BigInt oracle',()=>{
  const boundary=[MIN,MIN+1n,-(1n<<62n),-3n,-1n,0n,1n,3n,1n<<62n,MAX-1n,MAX];
  const pairs=boundary.flatMap(a=>boundary.map(b=>[a,b]));
  let state=0x61fd9c871aba013dn;
  const next=()=>{
    state^=(state<<13n)&MASK;
    state^=state>>7n;
    state^=(state<<17n)&MASK;
    state&=MASK;
    return wrap(state);
  };
  for(let k=0;k<10000;k++)pairs.push([next(),next()]);
  for(const [a,b] of pairs) {
    assert.equal(add(a,b),wrap(a+b),'wrapping_add '+a+','+b);
    assert.equal(sub(a,b),wrap(a-b),'wrapping_sub '+a+','+b);
    assert.equal(mul(a,b),wrap(a*b),'wrapping_mul '+a+','+b);
    assert.equal(canMul(a,b),a*b>=MIN&&a*b<=MAX,'can_mul '+a+','+b);
    const mayAdd=!(b>0n&&a>MAX-b)&&!(b<0n&&a<MIN-b);
    const maySub=!(b<0n&&a>MAX+b)&&!(b>0n&&a<MIN+b);
    assert.equal(mayAdd,a+b>=MIN&&a+b<=MAX);
    assert.equal(maySub,a-b>=MIN&&a-b<=MAX);
  }
});
test('guest runner fails closed without configured real compiler',()=>{
  const env={...process.env,ATTO_CORE_FOUR_LANE:'llvm-native'};
  for(const key of Object.keys(env))
    if(key.startsWith('ATTO_CORE_FOUR_') && key!=='ATTO_CORE_FOUR_LANE')delete env[key];
  const r=spawnSync(process.execPath,
    [new URL('../scripts/run-core-four-guest.mjs',import.meta.url).pathname],
    {env,encoding:'utf8',timeout:10000});
  assert.equal(r.status,1);
  const report=JSON.parse(r.stdout);
  assert.equal(report.certified_guest_passes,0);
  assert.equal(report.passed,0);
  assert.ok(report.failed>0);
});
