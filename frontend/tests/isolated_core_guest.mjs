import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, stat, rm } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const compiler = process.env.ATTOC || './target/debug/attoc';
const dir = await mkdtemp(join(tmpdir(), 'atto-isolated-core-'));
async function compile(id, source, flags = []) {
  const input = join(dir, id + '.atto'), out = join(dir, id + '.out');
  await writeFile(input, source);
  const r = spawnSync(compiler, [...flags, input, '-o', out], {encoding:'utf8', timeout:30000});
  assert.ifError(r.error); assert.equal(r.signal, null);
  return {r,out};
}
try {
  const src = 'pub fnc main():int {return 42;}';
  const plain = await compile('plain',src); assert.equal(plain.r.status,0,plain.r.stderr);
  const normal = new WebAssembly.Instance(new WebAssembly.Module(await readFile(plain.out)),{});
  assert.equal(normal.exports.main(),42n); assert.equal(normal.exports.panic_code.value,0);
  const guest = await compile('isolated',src,['--isolated-core']); assert.equal(guest.r.status,0,guest.r.stderr);
  const mod = new WebAssembly.Module(await readFile(guest.out));
  assert.deepEqual(WebAssembly.Module.imports(mod),[]);
  assert.deepEqual(WebAssembly.Module.exports(mod).map(e=>e.name).sort(),['main','memory']);
  const iso = new WebAssembly.Instance(mod,{}); assert.equal(iso.exports.main(),42n);
  assert.equal(iso.exports.memory.buffer.byteLength,65536); assert.throws(()=>iso.exports.memory.grow(1),RangeError);
  const rejected = await compile('stdout','pub fnc main():int {stdio.println(42); return 42;}',['--isolated-core']);
  assert.equal(rejected.r.status,1,rejected.r.stderr); assert.match(rejected.r.stderr,/E_ISOLATED_CORE_PROFILE/);
  await assert.rejects(stat(rejected.out),{code:'ENOENT'});
  console.log('PASS isolated/default ABI and stdout profile denial');
} finally { await rm(dir,{recursive:true,force:true}); }
