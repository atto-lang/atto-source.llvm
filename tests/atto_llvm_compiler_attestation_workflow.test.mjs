import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const text=readFileSync(new URL('../.github/workflows/atto-llvm-compiler-attestation.yml', import.meta.url),'utf8');
test('signer workflow exact path and identity',()=>{
  assert.match(text,/name: AttoLang LLVM compiler signed provenance/);
});
test('only main can sign, never a pull request',()=>{
  assert.match(text,/github\.event_name != 'pull_request' && github\.ref == 'refs\/heads\/main'/);
  assert.match(text,/github\.repository == 'ores-truffle-oreslang\/oreslang-source\.llvm'/);
});
test('read-only PR source checks cannot mint an attestation',()=>{
  const contract=text.split(/\n  source-only-contract:\n/)[1].split(/\n  signed-compiler:\n/)[0];
  assert.match(contract,/permissions:\n      contents: read/);
  assert.doesNotMatch(contract,/attestations: write|id-token: write/);
});
test('signed compiler job uses GitHub OIDC attestation',()=>{
  assert.match(text,/id-token: write/);
  assert.match(text,/attestations: write/);
  assert.match(text,/uses: actions\/attest@[0-9a-f]{40}/);
});
test('checkout pins source head without persisting tokens',()=>{
  assert.match(text,/ref: \$\{\{ github\.sha \}\}/);
  assert.match(text,/persist-credentials: false/);
  assert.match(text,/git rev-parse HEAD/);
});
test('two separately fetched private main protection fences',()=>{
  assert.equal(text.match(/--jq '\.protected'/g)?.length,2);
  assert.equal(text.match(/--jq '\.commit\.sha'/g)?.length,2);
});
test('build compiles native binary from source, not a cache',()=>{
  assert.match(text,/cmake --build "\$build" --target oreslang-llvmc/);
  assert.match(text,/cmake -G Ninja -S \./);
  assert.doesNotMatch(text,/actions\/download-artifact|git clone|gh repo clone/);
});
test('check ELF and SHA-256 before attestation',()=>{
  assert.match(text,/7f454c46/);
  assert.match(text,/sha256sum "\$build\/oreslang-llvmc"/);
});
test('attestation signs the compiler bytes, not an unsigned manifest',()=>{
  assert.match(text,/subject-path:/);
  assert.match(text,/atto-llvmc-attested-build\/oreslang-llvmc/);
});
test('source repo retains its own artifact',()=>{
  assert.match(text,/uses: actions\/upload-artifact@[0-9a-f]{40}/);
  assert.doesNotMatch(text,/git push|scp |rsync |curl .*upload/);
});
