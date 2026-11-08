// Fault injection into a *compiled* import-free wasm32 actor module.
// This is a test helper, not an implementation of a guest sandbox.
import {createHash} from 'node:crypto';

const LIMIT = 0xffff_ffff;
const MAGIC = Uint8Array.from([0,97,115,109,1,0,0,0]);
const enc = new TextEncoder();
const dec = new TextDecoder('utf-8', {fatal:true});
const ON_MESSAGE = 'oreslang_wasm_actor_on_message';

function need(valid, reason) {if (!valid) throw Error('WASM_MUTATION_DENIED: ' + reason);}
function readU32(bytes, pointer, end) {
  let value=0;
  for (let i=0;i<5;i++) {
    need(pointer.i < end,'truncated u32 LEB');
    const b=bytes[pointer.i++];
    if (i===4) need((b & 0xf0)===0,'u32 LEB overflow');
    value += (b & 0x7f) * (2 ** (7*i));
    if ((b & 0x80)===0) {need(value<=LIMIT,'u32 overflow'); return value;}
  }
  throw Error('WASM_MUTATION_DENIED: overlong u32 LEB');
}
function writeU32(value) {
  need(Number.isInteger(value) && value>=0 && value<=LIMIT,'u32 out of range');
  const out=[];
  do {
    let b=value & 0x7f;
    value=Math.floor(value/128);
    if (value!==0) b |= 0x80;
    out.push(b);
  } while(value!==0);
  return Uint8Array.from(out);
}
const concat = (...blocks) => Buffer.concat(blocks.map(x=>Buffer.from(x)));
function inspect(bytes) {
  const source=Buffer.from(bytes);
  need(source.length>=8 && source.subarray(0,8).equals(Buffer.from(MAGIC)),'not wasm32 binary');
  need(WebAssembly.validate(source),'baseline Wasm is invalid');
  need(WebAssembly.Module.imports(new WebAssembly.Module(source)).length===0,'host imports forbidden');
  const sections=[];
  const p={i:8};
  while(p.i<source.length) {
    const sectionStart=p.i;
    const id=source[p.i++];
    const length=readU32(source,p,source.length);
    const start=p.i;
    const end=start+length;
    need(end<=source.length,'section outside file');
    sections.push({id,sectionStart,start,end});
    p.i=end;
  }
  return {source,sections};
}
function only(sections,id) {
  const found=sections.filter(s=>s.id===id);
  need(found.length===1,'expected exactly one section '+id);
  return found[0];
}
function exportsOf(source,section) {
  const p={i:section.start};
  const count=readU32(source,p,section.end);
  need(count<=4096,'excessive exports');
  const exports=[];
  for (let n=0;n<count;n++) {
    const len=readU32(source,p,section.end);
    const first=p.i;
    need(len<=section.end-first,'truncated export');
    const name=dec.decode(source.subarray(first,first+len));
    p.i+=len;
    need(p.i<section.end,'truncated export kind');
    const kind=source[p.i++];
    const index=readU32(source,p,section.end);
    exports.push({name,kind,index,first,len});
  }
  need(p.i===section.end,'export section trailing bytes');
  return exports;
}
function messageExport(source,sections) {
  const matches=exportsOf(source,only(sections,7)).filter(e=>e.name===ON_MESSAGE && e.kind===0);
  need(matches.length===1,'ambiguous or missing compiled on_message export');
  return matches[0];
}
export function mutateActorWasmExport(wasm) {
  const {source,sections}=inspect(wasm);
  const entry=messageExport(source,sections);
  need(entry.len===enc.encode(ON_MESSAGE).length,'unexpected export encoding');
  const output=Buffer.from(source);
  // Same-length export rename keeps the module valid and forces typed ABI denial.
  output[entry.first+entry.len-1] = 'f'.charCodeAt(0);
  need(WebAssembly.validate(output),'export mutation invalidated wasm');
  need(WebAssembly.Module.imports(new WebAssembly.Module(output)).length===0,'mutated module gained imports');
  return output;
}
export function mutateActorWasmOutOfBounds(wasm) {
  const {source,sections}=inspect(wasm);
  const entry=messageExport(source,sections);
  const funcSection=only(sections,3);
  const fp={i:funcSection.start};
  const fnCount=readU32(source,fp,funcSection.end);
  need(fnCount>entry.index && fnCount<=4096,'nonlocal or imported function export');
  for(let n=0;n<fnCount;n++) readU32(source,fp,funcSection.end);
  need(fp.i===funcSection.end,'function section trailing bytes');
  const section=only(sections,10);
  const cursor={i:section.start};
  const codeCount=readU32(source,cursor,section.end);
  need(codeCount===fnCount,'function/code body mismatch');
  const bodies=[];
  for(let n=0;n<codeCount;n++) {
    const size=readU32(source,cursor,section.end);
    const begin=cursor.i;
    const end=begin+size;
    need(end<=section.end,'truncated body');
    let body=source.subarray(begin,end);
    if(n===entry.index){
      const lp={i:0};
      const groups=readU32(body,lp,body.length);
      need(groups<=4096,'excessive local groups');
      for(let j=0;j<groups;j++){
        readU32(body,lp,body.length); // local count
        need(lp.i<body.length,'truncated local type');
        lp.i++; // locally declared numeric or ref type
      }
      need(lp.i<body.length,'empty function instruction stream');
      // i32.const -1; i64.load align=0 offset=0; drop.
      // This is valid guest bytecode but traps at effective address 0xffffffff.
      const fault=Uint8Array.from([0x41,0x7f,0x29,0x00,0x00,0x1a]);
      body=concat(body.subarray(0,lp.i),fault,body.subarray(lp.i));
    }
    bodies.push(concat(writeU32(body.length),body));
    cursor.i=end;
  }
  need(cursor.i===section.end,'code section trailing bytes');
  const payload=concat(writeU32(codeCount),...bodies);
  const codeSection=concat(Uint8Array.of(10),writeU32(payload.length),payload);
  const output=concat(source.subarray(0,section.sectionStart),codeSection,source.subarray(section.end));
  need(WebAssembly.validate(output),'out-of-bounds mutation invalidated wasm');
  need(WebAssembly.Module.imports(new WebAssembly.Module(output)).length===0,'mutated module gained imports');
  return output;
}
export const wasmSha256 = bytes => createHash('sha256').update(bytes).digest('hex');
