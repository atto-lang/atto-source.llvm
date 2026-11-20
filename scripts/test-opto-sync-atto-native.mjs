#!/usr/bin/env node
// Compiler-backed Opto Sync AttoLang scalar guards. LLVM-native guest execution.
// This is NOT a sync SDK, network replay, actor certification or backend parity.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {
  lstatSync, mkdtempSync, readFileSync, realpathSync, rmSync, statSync, writeFileSync
} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dir = join(root, 'tests/fixtures/opto-sync-atto-v1');
const manifest = JSON.parse(readFileSync(join(dir, 'manifest.v1.json'), 'utf8'));
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const blobSha1 = bytes => createHash('sha1')
  .update(Buffer.from('blob ' + bytes.length + '\0')).update(bytes).digest('hex');
const run = (bin, args, options={}) => {
  const result = spawnSync(bin, args, {
    cwd: root, encoding: 'utf8', timeout: 120_000,
    maxBuffer: 2 * 1024 * 1024, ...options
  });
  assert.ifError(result.error);
  assert.equal(result.signal, null, 'compiler or guest signal/crash from ' + bin);
  assert.notEqual(result.status, null, 'missing numeric process status for ' + bin);
  return result;
};
const checkedFile = path => {
  const p = resolve(path);
  assert.equal(realpathSync(p), p, 'symlink or redirected executable/source: ' + p);
  assert(lstatSync(p).isFile(), 'not a regular file: ' + p);
  return p;
};
const head = run('git', ['rev-parse','HEAD']).stdout.trim();
const tree = run('git', ['rev-parse','HEAD^{tree}']).stdout.trim();
const expectedHead = process.env.ATTO_EXPECTED_SHA;
if (expectedHead) assert.equal(head, expectedHead, 'not the requested exact source head');
assert.match(head, /^[a-f0-9]{40}$/);
assert.match(tree, /^[a-f0-9]{40}$/);

assert.equal(manifest.schema, 'opto-sync.atto.scalar-native.v1');
assert.equal(manifest.backend, 'llvm');
assert.equal(manifest.release_claim, 'not-an-opto-sync-sdk');
assert.deepEqual(manifest.required_optimizations, ['O0','O2']);
assert.equal(manifest.guest_exit, 42);
assert.equal(manifest.cases.length, 7);
assert.equal(manifest.cases.filter(x=>x.kind==='positive').length, 5);
assert.equal(manifest.cases.filter(x=>x.kind==='compile-negative').length, 2);
const ids = new Set();
for (const fixture of manifest.cases) {
  assert.match(fixture.id, /^[a-z][a-z0-9-]+$/);
  assert(!ids.has(fixture.id), 'duplicate fixture identity');
  ids.add(fixture.id);
  assert.equal(fixture.path, 'tests/fixtures/opto-sync-atto-v1/' + fixture.id + '.atto');
  assert(['positive','compile-negative'].includes(fixture.kind));
  assert.equal(fixture.expected_exit, fixture.kind==='positive' ? 42 : null);
  assert.match(fixture.git_blob_sha1, /^[0-9a-f]{40}$/);
  const file = checkedFile(join(root, fixture.path));
  assert.equal(blobSha1(readFileSync(file)), fixture.git_blob_sha1,
    'unreviewed guest source bytes: ' + fixture.id);
}
if (process.argv.slice(2).includes('--verify-source-only')) {
  console.log('OPTO_SYNC_ATTO_SOURCE_ONLY=7_BLOB_PINS_PASS');
  console.log('GUEST_COMPILATION=NOT_EXECUTED');
  process.exit(0);
}
assert.equal(process.argv.length, 2, 'unknown arguments');
const compiler = checkedFile(process.env.ORESLANG_LLVMC ?? join(root, 'build/oreslang-llvmc'));
const auditor = checkedFile(process.env.ORES_NO_GC_IR_AUDIT ?? join(root, 'build/oreslang-no-gc-ir-audit'));
const compilerHash = sha256(readFileSync(compiler));
const auditorHash = sha256(readFileSync(auditor));
const scratch = mkdtempSync(join(tmpdir(), 'opto-atto-llvm-'));
const cases = [];
let positive = 0, negative = 0;
try {
  for (const opt of manifest.required_optimizations) {
    for (const fixture of manifest.cases) {
      const source = checkedFile(join(root, fixture.path));
      const sourceHash = sha256(readFileSync(source));
      const binary = join(scratch, fixture.id + '-' + opt + '.native');
      const compile = run('bash', [
        join(root,'scripts/compile-aot.sh'), source, binary
      ], {env:{
        ...process.env, ORESLANG_LLVMC:compiler, ORES_NO_GC_IR_AUDIT:auditor,
        ORESLANG_LLVM_AOT_OPT_LEVEL:opt.slice(1), ORESLANG_AOT_LTO:'off'
      }});
      if (fixture.kind === 'positive') {
        assert.equal(compile.status, 0,
          fixture.id + '/' + opt + ' compiler rejected valid guest: ' + compile.stderr.slice(0,1600));
        checkedFile(binary);
        assert(statSync(binary).size>0, 'empty guest executable');
        const guest = run(binary, [], {timeout:30_000});
        assert.equal(guest.status, 42,
          fixture.id + '/' + opt + ' wrong native guest status: ' + guest.status);
        assert.equal(guest.stdout,'', 'guest emitted unexpected stdout');
        assert.equal(guest.stderr,'', 'guest emitted unexpected stderr');
        cases.push({
          id:fixture.id, optimization:opt, kind:'positive',
          source_sha256:sourceHash, source_git_blob:fixture.git_blob_sha1,
          compiler_exit:compile.status, guest_exit:guest.status,
          artifact_sha256:sha256(readFileSync(binary)), executed:true
        });
        positive++;
        continue;
      }
      const front = run(compiler, ['--target=host',source]);
      assert.notEqual(front.status, 0,
        fixture.id + '/' + opt + ': unsupported source generated IR');
      assert.equal(front.stdout,'', 'invalid guest published LLVM IR');
      assert(front.stderr.trim().length > 0, 'missing compiler negative diagnostic');
      assert(!/segmentation fault|internal compiler error|stack trace|assertion failed/i.test(front.stderr),
        'compiler crashed rather than semantically rejecting negative source');
      assert.notEqual(compile.status, 0,
        fixture.id + '/' + opt + ': invalid source produced native executable');
      // The compile script publishes the target path atomically on success.
      assert.throws(() => lstatSync(binary), {code:'ENOENT'},
        'compile-negative fixture left behind a native artifact');
      cases.push({
        id:fixture.id, optimization:opt, kind:'compile-negative',
        source_sha256:sourceHash, source_git_blob:fixture.git_blob_sha1,
        compiler_exit:front.status, guest_exit:null, artifact_sha256:null,
        executed:false, failure_stage:'compiler-rejection'
      });
      negative++;
    }
  }
  assert.equal(positive, 10);
  assert.equal(negative, 4);
  const receipt = {
    schema:'opto-sync.atto.native-llvm-execution.v1',
    classification:'COMPILER_GENERATED_LLVM_NATIVE_SCALAR_GUEST_ONLY',
    atto_cross_backend_parity:'NOT_CERTIFIED',
    atto_product_sync_sdk:'NOT_IMPLEMENTED',
    git_head:head, git_tree:tree, compiler_sha256:compilerHash,
    no_gc_ir_auditor_sha256:auditorHash,
    workflow_run_id:process.env.GITHUB_RUN_ID ?? null,
    workflow_run_attempt:process.env.GITHUB_RUN_ATTEMPT ?? null,
    positive_executed:positive, compile_negative_refused:negative, cases
  };
  const output = process.env.ATTO_OPTO_RECEIPT;
  if (output) writeFileSync(output, JSON.stringify(receipt,null,2)+'\n', {flag:'wx'});
  console.log('OPTO_ATTO_LLVM_GUEST=PASS O0_O2_POSITIVE=10 COMPILER_NEGATIVE=4');
  console.log(JSON.stringify(receipt));
} finally {
  if (!process.env.ATTO_KEEP_GUEST_WORK) rmSync(scratch,{recursive:true,force:true});
}
