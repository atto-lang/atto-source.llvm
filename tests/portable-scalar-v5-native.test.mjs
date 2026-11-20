// Actual LLVM IR -> Clang -> ELF guest execution for the shared scalar-v5 corpus.
// Exit code 42 is the bounded portable oracle; full signed-i64 result is
// compared *inside compiled .atto*, not truncated to the host's 8-bit exit code.
import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {readFileSync, writeFileSync, mkdtempSync, existsSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const manifestBytes = readFileSync(new URL('../contracts/parity/portable-scalar-v5-20261120.json', import.meta.url));
const manifest = JSON.parse(manifestBytes.toString('utf8'));
const canonicalBlob = 'e9b370f81efef07e7f2818d007486fea82c95b3d';
const blobSha = bytes => createHash('sha1').update('blob ' + bytes.length + '\0').update(bytes).digest('hex');
const compiler = resolve(process.env.ATTO_LLVM ?? 'target/release/atto-llvm');
const clang = process.env.CLANG ?? 'clang';

function processResult(executable, args, opts={}) {
  const result = spawnSync(executable, args, {
    encoding:'utf8', timeout:12000, maxBuffer:2 << 20, ...opts
  });
  assert.equal(result.error, undefined, 'missing executable, launch error or timeout: ' + executable);
  return result;
}
function checkedIR(text) {
  assert.match(text, /@llvm\.(?:sadd|ssub|smul)\.with\.overflow\.i64/,
    'IR must contain actual checked signed i64 arithmetic');
}
function compileGuest(source, sourcePath, irPath) {
  writeFileSync(sourcePath, source, {flag:'wx'});
  const result = processResult(compiler, [sourcePath, '-o', irPath, '--emit-llvm']);
  return result;
}
function linkELF(irPath, exePath) {
  const result = processResult(clang, ['-x', 'ir', '-O2', '-fno-lto', '-o', exePath, irPath]);
  assert.equal(result.status, 0, 'Clang failed on Atto LLVM IR: ' + result.stderr);
  assert.equal(result.signal, null);
  assert.deepEqual(readFileSync(exePath).subarray(0,4), Buffer.from([0x7f,0x45,0x4c,0x46]),
    'not an ELF native executable');
}

test('scalar v5 has its own immutable Atto operator Git blob (legacy v4 retained)', () => {
  assert.equal(blobSha(manifestBytes), canonicalBlob);
  assert.equal(manifest.schema_version, '2026-11-20.scalar-v5');
  assert.equal(manifest.scope, 'import-free-pure-i64');
  assert.deepEqual([manifest.positive.length,manifest.compile_reject.length,manifest.checked_overflow.length], [13,4,4]);
  const all = [...manifest.positive,...manifest.compile_reject,...manifest.checked_overflow];
  assert.equal(new Set(all.map(x => x.id)).size, 21);
  for (const row of all) {
    assert.match(row.id, /^[a-z0-9]+(?:-[a-z0-9]+)*$/);
    assert.match(row.expression, /^[0-9+\-*() %^\t]+$/);
    assert.ok(row.expression.length > 0 && row.expression.length < 200);
  }
});
for (const [kind, rows] of Object.entries({
  positive:manifest.positive,
  compile_reject:manifest.compile_reject,
  checked_overflow:manifest.checked_overflow
})) {
  for (const row of rows) {
    test('actual native ELF scalar v5 ' + kind + ' ' + row.id, () => {
      const dir = mkdtempSync(join(tmpdir(), 'atto-llvm-scalar-v5-'));
      try {
        const src = join(dir, 'case.atto'), ir = join(dir, 'case.ll'), exe = join(dir, 'case.exe');
        // Linux process exit codes retain only 8 bits. Compare exact signed-i64
        // arithmetic in generated guest code and return 42 iff it matches.
        const source = kind === 'positive'
          ? 'pub fnc main(): int { return 42 + ((' + row.expression + ') - (' + row.expected + ')); }\n'
          : 'pub fnc main(): int { return ' + row.expression + '; }\n';
        const compiled = compileGuest(source, src, ir);
        if (kind === 'compile_reject') {
          assert.equal(compiled.status, 1, 'compiler must reject invalid Atto source');
          assert.equal(compiled.signal, null, 'compiler crashed rather than rejecting source');
          assert.match(compiled.stderr, /E_[A-Z0-9_]+/, 'source-phase diagnostic required');
          assert.equal(existsSync(ir), false, 'rejected guest published IR');
          return;
        }
        assert.equal(compiled.status, 0, 'native compiler refused source: ' + compiled.stderr);
        assert.equal(compiled.signal, null, 'native compiler crashed');
        assert.equal(existsSync(ir), true, 'native LLVM compiler produced no IR');
        const llvm = readFileSync(ir, 'utf8');
        checkedIR(llvm);
        linkELF(ir, exe);
        const ran = processResult(exe, [], {cwd:dir});
        if (kind === 'positive') {
          assert.equal(ran.signal, null, 'native guest crashed on valid case');
          assert.equal(ran.status, 42, 'full i64 guest oracle mismatch (return !=42)');
          assert.equal(ran.stderr, '', 'guest emitted unexpected diagnostics');
        } else {
          assert.ok(ran.signal === 'SIGILL' || ran.signal === 'SIGTRAP' || ran.signal === 'SIGABRT',
            'expected native llvm.trap(), not wrapped output, nonzero return, or timeout: ' +
              JSON.stringify({status:ran.status,signal:ran.signal,stderr:ran.stderr}));
          assert.match(llvm, /call void @llvm\.trap\(\)/, 'overflow lacks native trap instruction');
        }
      } finally {
        rmSync(dir, {recursive:true, force:true});
      }
    });
  }
}

test('single caret returns from the containing named callable', () => {
  const dir = mkdtempSync(join(tmpdir(), 'atto-llvm-return-caret-'));
  try {
    const src = join(dir, 'return-caret.atto');
    const ir = join(dir, 'return-caret.ll');
    const exe = join(dir, 'return-caret.exe');
    const compiled = compileGuest('pub fnc main(): int { ^ (40 + 2); }\n', src, ir);
    assert.equal(compiled.status, 0, 'compiler rejected valid return caret: ' + compiled.stderr);
    assert.equal(compiled.signal, null);
    assert.equal(existsSync(ir), true, 'no emitted LLVM IR for return caret');
    checkedIR(readFileSync(ir, 'utf8'));
    linkELF(ir, exe);
    const ran = processResult(exe, [], {cwd:dir});
    assert.equal(ran.status, 42, 'caret return did not propagate the value');
    assert.equal(ran.signal, null);
    assert.equal(ran.stdout, '');
    assert.equal(ran.stderr, '');
  } finally {
    rmSync(dir, {recursive:true, force:true});
  }
});

test('boolean operands do not overload integer ^^ XOR', () => {
  const dir = mkdtempSync(join(tmpdir(), 'atto-llvm-bool-xor-'));
  try {
    const src = join(dir, 'bool-xor.atto');
    const ir = join(dir, 'bool-xor.ll');
    const compiled = compileGuest('pub fnc main(): int { return true ^^ false; }\n', src, ir);
    assert.equal(compiled.status, 1);
    assert.equal(compiled.signal, null);
    assert.match(compiled.stderr, /E_TYPE|E_UNSUPPORTED/);
    assert.equal(existsSync(ir), false, 'invalid boolean XOR published native artifact');
  } finally {
    rmSync(dir, {recursive:true, force:true});
  }
});
