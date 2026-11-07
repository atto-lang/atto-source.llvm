import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { test } from 'node:test';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const gate = resolve(repo, 'scripts/verify-atto-gpu-saxpy-hardware.sh');
const cmake = resolve(repo, 'tests/cmake/verify-atto-gpu-saxpy-physical-v1.cmake');
const git = spawnSync('git', ['rev-parse', 'HEAD'], {
  cwd: repo, encoding: 'utf8'
});
assert.equal(git.status, 0, 'source-authenticity tests require Git checkout');
const head = git.stdout.trim();
assert.match(head, /^[0-9a-f]{40}$/);

function run(program, args, env = {}) {
  return spawnSync(program, args, {
    cwd: repo,
    encoding: 'utf8',
    timeout: 10000,
    env: {
      ...process.env,
      // Tests are deliberately denied any valid hosted Git credentials.
      GITHUB_RUN_ID: '191234',
      GITHUB_REPOSITORY: 'ores-truffle-oreslang/oreslang-source.llvm',
      GITHUB_RUN_ATTEMPT: '1',
      ATTO_EXPECTED_PR_HEAD_SHA: head,
      ...env
    }
  });
}
function blocked(result, code, message) {
  assert.equal(result.error, undefined, message + ': process failed to spawn');
  assert.equal(result.status, code, message);
  const output = (result.stdout ?? '') + (result.stderr ?? '');
  assert.doesNotMatch(output, /ATTO_SAXPY_GPU_RECEIPT_V1/,
                      message + ': forged receipt must not be published');
  assert.doesNotMatch(output, /ATTO_SAXPY_PHYSICAL_GPU_PASSED/,
                      message + ': forged device pass must not be published');
}

test('hardware gate refuses supplied compiler/probe/fixture binaries', () => {
  blocked(run('bash', [gate, '/bin/true', '/bin/true', '/dev/null']),
          64, 'arbitrary caller executable');
});
test('hardware gate refuses partial arguments without executing a probe', () => {
  blocked(run('bash', [gate, '/bin/true']),
          64, 'partial argument substitution');
});
test('hardware gate refuses mismatched immutable head', () => {
  blocked(run('bash', [gate], {
    ATTO_EXPECTED_PR_HEAD_SHA: '0'.repeat(40)
  }), 70, 'wrong pull request head');
});
test('hardware gate rejects absent hosted pull request head', () => {
  blocked(run('bash', [gate], {
    ATTO_EXPECTED_PR_HEAD_SHA: ''
  }), 70, 'absent hosted head');
});
test('hardware gate rejects nonexistent hosted attempt', () => {
  blocked(run('bash', [gate], {
    GITHUB_RUN_ATTEMPT: ''
  }), 70, 'missing run attempt');
});
test('hardware gate rejects zero attempt', () => {
  blocked(run('bash', [gate], {
    GITHUB_RUN_ATTEMPT: '0'
  }), 70, 'zero run attempt');
});
test('hardware gate rejects malformed run attempt', () => {
  blocked(run('bash', [gate], {
    GITHUB_RUN_ATTEMPT: '2a'
  }), 70, 'invalid run attempt');
});
test('hardware gate rejects foreign repository identity', () => {
  blocked(run('bash', [gate], {
    GITHUB_REPOSITORY: 'other-org/other-repo'
  }), 70, 'foreign repository');
});
test('hardware gate refuses a CMake probe with no authenticated inputs', () => {
  const result = run('cmake', ['-P', cmake]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr ?? '', /requires compiler, probe, source/);
  assert.doesNotMatch(result.stdout ?? '', /PHYSICAL_GPU_EXECUTED/);
});
test('hardware CMake probe rejects missing compiler before device claims', () => {
  const result = run('cmake', [
    '-DCOMPILER=/definitely/missing/oreslang-llvmc',
    '-DPROBE=/definitely/missing/saxpy',
    '-DSOURCE=/definitely/missing/kernel.atto',
    '-P', cmake
  ]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr ?? '', /missing executable or fixture/);
  assert.doesNotMatch(result.stdout ?? '', /PHYSICAL_GPU_EXECUTED/);
});
