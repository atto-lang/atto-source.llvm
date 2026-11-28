import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const mod=new WebAssembly.Module(readFileSync(process.argv[2]));assert.deepEqual(WebAssembly.Module.imports(mod),[]);
const e=new WebAssembly.Instance(mod,{}).exports;
assert.equal(e.atto_bounded_fields_v2(),4);assert.equal(BigInt.asUintN(64,e.atto_bounded_schema_v2()),0xdb14dea8243c195bn);
const base=Number(e.__heap_base.value),input=base,output=base+69632,decoded=output+65536;
assert.ok(decoded+69632<=e.memory.buffer.byteLength);
const mem=new Uint8Array(e.memory.buffer);assert.throws(()=>e.memory.grow(1));
function slots(n,yes,s,data){const b=new Uint8Array(64+s.length+data.length),d=new DataView(b.buffer);for(const [i,v] of [[0,n],[16,yes?1n:0n],[32,64n],[40,BigInt(s.length)],[48,BigInt(64+s.length)],[56,BigInt(data.length)]])d.setBigUint64(i,BigInt.asUintN(64,v),true);b.set(s,64);b.set(data,64+s.length);return b;}
const golden=Uint8Array.from(Buffer.from('4154544f53543032025b193c24a8de14db0400010000000000000080020103080000004100c3a9f09f9880040300000000ff8074c4cc420e041ac0','hex'));
function enc(b){mem.set(b,input);const n=e.atto_bounded_encode_v2(input,b.length,output,65536);assert.ok(n>0);return mem.slice(output,output+n);}
function dec(b){mem.set(b,input);const n=e.atto_bounded_decode_v2(input,b.length,decoded,69632);assert.ok(n>=0);return mem.slice(decoded,decoded+n);}
const original=slots(-(1n<<63n),true,Uint8Array.from([65,0,195,169,240,159,152,128]),Uint8Array.from([0,255,128]));assert.deepEqual(enc(original),golden);assert.deepEqual(dec(golden),original);
for(let i=-5000n;i<=5000n;i++){const b=slots(i,i%2n===0n,new Uint8Array(Number((i+5000n)%257n)).fill(97),Uint8Array.of(Number(BigInt.asUintN(8,i)),0,255));assert.deepEqual(dec(enc(b)),b);}
const sentinel=new Uint8Array(80).fill(99);
function deny(b){mem.set(b,input);mem.set(sentinel,decoded);assert.equal(e.atto_bounded_decode_v2(input,b.length,decoded,80),-1);assert.deepEqual(mem.slice(decoded,decoded+80),sentinel);}
for(let i=0;i<golden.length;i++){deny(golden.slice(0,i));for(let j=0;j<8;j++){const b=golden.slice();b[i]^=1<<j;deny(b);}}
for(const i of [8,29,31,37]){const b=golden.slice();b[i]=255;let h=0xcbf29ce484222325n;for(const x of b.subarray(0,-8))h=BigInt.asUintN(64,(h^BigInt(x))*0x100000001b3n);new DataView(b.buffer).setBigUint64(b.length-8,h,true);deny(b);}
const bad=slots(0n,false,Uint8Array.of(0xc0,0x80),new Uint8Array());mem.set(bad,input);mem.set(sentinel,output);assert.equal(e.atto_bounded_encode_v2(input,bad.length,output,80),-1);assert.deepEqual(mem.slice(output,output+80),sentinel);
mem.set(original,input);assert.equal(e.atto_bounded_encode_v2(input,original.length,input,original.length),-1);assert.deepEqual(mem.slice(input,input+original.length),original);
const max=slots(0n,false,new Uint8Array(16384).fill(97),new Uint8Array(16384).fill(255));assert.deepEqual(dec(enc(max)),max);
console.log('LLVM_BRIDGE_WASM_V2_PASS roundtrips=10001 bitflips=472 truncations=59 imports=0');
