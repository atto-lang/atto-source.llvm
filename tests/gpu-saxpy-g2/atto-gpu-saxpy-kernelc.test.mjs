import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const compiler = process.env.ATTO_GPU_KERNELC;
const clang = process.env.ATTO_GPU_CLANG ?? 'clang';
assert.ok(compiler, 'ATTO_GPU_KERNELC must refer to the locally compiled native tool');
const source = readFileSync(new URL('./saxpy.atto', import.meta.url), 'utf8');

function run(input) {
  const folder = mkdtempSync(join(tmpdir(), 'atto-gpu-g2-'));
  try {
    const atto = join(folder, 'kernel.atto');
    const cl = join(folder, 'kernel.cl');
    writeFileSync(atto, input);
    const result = spawnSync(compiler, [atto, cl], {encoding: 'utf8', timeout: 15000});
    const artifact = existsSync(cl) ? readFileSync(cl, 'utf8') : null;
    const device = artifact === null ? null : spawnSync(clang, ['-x','cl','-cl-std=CL1.2','-fsyntax-only',cl], {encoding:'utf8',timeout:15000});
    const spir = artifact === null ? null : spawnSync(clang, ['-target','spir64-unknown-unknown','-x','cl','-cl-std=CL1.2','-S','-emit-llvm','-o','-',cl], {encoding:'utf8', timeout:15000});
    return {result, artifact, device, spir};
  } finally {
    rmSync(folder, {recursive:true, force:true});
  }
}

const positives = [
  ['canonical source', source],
  ['renamed disjoint bindings', source.replaceAll('scale', 'alpha').replaceAll('out', 'result').replaceAll('const u64 i', 'const u64 lane').replaceAll('[i]', '[lane]').replaceAll('if i >= n', 'if lane >= n')],
  ['comment and whitespace variations', source.replace('return;', '/* bounded */ return;').replace('pub gpu fnc', 'pub   gpu\nfnc')],
  ['optional final return', source.replace('    return;\n}', '}')],
];
for (const [name, code] of positives) test(`G2 positive: ${name}`, () => {
  const {result, artifact, device, spir} = run(code);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /G2_SOURCE_TO_OPENCL_C_ONLY/);
  assert.match(artifact, /__kernel void attolang_saxpy_v1/);
  assert.match(artifact, /if \(i >= n\) return;/);
  assert.equal(device.status, 0, device.stderr);
  assert.equal(device.stdout, '');
  assert.equal(spir.status, 0, spir.stderr);
  assert.match(spir.stdout, /target triple = \"spir64-unknown-unknown\"/);
  assert.match(spir.stdout, /spir_kernel void @attolang_saxpy_v1/);
  assert.match(spir.stdout, /get_global_id/);
});

const cases = [
  ['host launch tail is not yet compiled', s => s + '\nfnc main(): void { launch saxpy(); }'],
  ['CUDA chevron', s => s.replace('gpu.global_id(0)', 'gpu<<<1,256>>>')],
  ['wrong device axis', s => s.replace('gpu.global_id(0)', 'gpu.global_id(1)')],
  ['wrong element scalar', s => s.replace('GpuArray<f32> x', 'GpuArray<f64> x')],
  ['host buffer capture', s => s.replace('GpuArray<f32> x', 'HostBuffer<f32> x')],
  ['missing exclusive out', s => s.replace('GpuArray<f32> mut out', 'GpuArray<f32> out')],
  ['duplicate input binding', s => s.replace('GpuArray<f32> y', 'GpuArray<f32> x')],
  ['duplicate index binding', s => s.replace('const u64 i', 'const u64 n').replaceAll('[i]', '[n]').replace('if i >= n', 'if n >= n')],
  ['missing bounds guard', s => s.replace('if i >= n { return; }', '')],
  ['reversed bounds', s => s.replace('if i >= n', 'if n >= i')],
  ['stale scalar reference', s => s.replace('scale * x[i]', 'beta * x[i]')],
  ['writes to input', s => s.replace('out[i] =', 'x[i] =')],
  ['different math', s => s.replace(' + y[i]', ' - y[i]')],
  ['wrong math order', s => s.replace('scale * x[i] + y[i]', 'y[i] + scale * x[i]')],
  ['host await', s => s.replace('return;\n}', 'await host(); return;\n}')],
  ['actor message', s => s.replace('return;\n}', 'actor.send(); return;\n}')],
  ['dynamic allocation', s => s.replace('return;\n}', 'new Array(); return;\n}')],
  ['second kernel declaration', s => s + '\npub gpu fnc evil() => void {}'],
  ['unclosed block comment', s => s + '\n/* never closed'],
  ['string literal', s => s.replace('return;', '"host"; return;')],
  ['non-ASCII extension', s => s + '\nλ'],
  ['oversize source', s => s + ' '.repeat(65537)],
  ['reserved parser keyword masquerading as name', s => s.replaceAll('scale', 'await')],
];
for (const [name, mutate] of cases) test(`G2 fail closed: ${name}`, () => {
  const {result, artifact} = run(mutate(source));
  assert.notEqual(result.status, 0, `${name}: unexpectedly lowered unsafe source`);
  assert.equal(artifact, null, `${name}: created output artifact despite failed admission`);
  assert.match(result.stderr, /GPU-KERNEL-G2-001/);
});

test('OpenCL output reproducible across reformatting and binder renaming', () => {
  const a = run(source), b = run(positives[1][1]);
  assert.equal(a.result.status, 0);
  assert.equal(b.result.status, 0);
  assert.equal(a.artifact, b.artifact);
});
