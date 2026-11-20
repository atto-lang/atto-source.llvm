// HOST-ONLY source binding audit. Does not execute .ores guest compilers.
import test from 'node:test';
import assert from 'node:assert/strict';
import {existsSync,readFileSync} from 'node:fs';

const manifest=JSON.parse(readFileSync('contracts/oresllm-parity/v1/scalar.json','utf8'));
const POS=manifest.positive, OVER=manifest.checked_overflow, BAD=manifest.compile_reject;
function read(path){assert.equal(existsSync(path),true,'missing '+path);return readFileSync(path,'utf8');}
const expectedFixtures={
  'precedence':'pub fnc main(): int { return 42 + ((2 + 3 * 4) - 14); }\n',
  'signed-product':'pub fnc main(): int { return 42 + ((-7 * -6) - 42); }\n',
  'scalar-gradient':'pub fnc main(): int { return 2 * ((5 * 3 + 1) - 9) * 3; }\n',
  'grouped-negative':'pub fnc main(): int { return 42 + ((-5 * (3 + 4)) + 35); }\n',
  'nested-precedence':'pub fnc main(): int { return 42 + ((1 + (2 * (3 + 4))) - 15); }\n',
  'max-minus-one':'pub fnc main(): int { return 43 + ((9223372036854775807 - 1) - 9223372036854775807); }\n',
  'zero-product':'pub fnc main(): int { return 42 + ((-7 * 0) - 0); }\n',
  'nested-arithmetic':'pub fnc main(): int { return 42 + (((2 + 3) * (10 - 2)) - 40); }\n',
  'subtract-self':'pub fnc main(): int { return 42 + ((9223372036854775807 - 9223372036854775807) - 0); }\n',
  'zero-add':"pub fnc main(): int { return 42 + (0 + 0); }\n",
  'signed-subtract':"pub fnc main(): int { return 42 + ((1 - 9) + 8); }\n",
  'left-associative':"pub fnc main(): int { return 42 + (((17 - 2) * 3) - 45); }\n",
  'zero-times-max':"pub fnc main(): int { return 42 + ((0 * 9223372036854775807) - 0); }\n",
  'over-2pow53':"pub fnc main(): int { return 42 + ((9007199254740993 + 1) - 9007199254740994); }\n",
  "min-max-cancel":"pub fnc main(): int { return 42 + ((-9223372036854775808 + 9223372036854775807) + 1); }\n",
  "near-square":"pub fnc main(): int { return 42 + ((3037000499 * 3037000499) - 9223372030926249001); }\n",
  "over53-cancel":"pub fnc main(): int { return 42 + ((9007199254740993 - 9007199254740992) - 1); }\n"
};
const backend=existsSync('pom.xml')?'java':
  existsSync('CMakeLists.txt')?'llvm':
  existsSync('verification/Ores.Net.Tests/CrossBackendCheckedI64ParityTests.cs')?'net':
  existsSync('Cargo.toml')?'cranelift':
  existsSync('scripts/compile-ores.mjs')?'wasm':
  existsSync('tools/native-guest-gate.mjs')?'conformance':'unknown';

test('backend is explicit, unknown backend cannot borrow conformance credit',()=>{
  assert.notEqual(backend,'unknown');
  assert.equal(POS.length,17);assert.equal(OVER.length,6);assert.equal(BAD.length,5);
});
test('backend-specific authored native case inventory is wired (not executed)',()=>{
  if(backend==='java'||backend==='llvm'){
    const cases=JSON.parse(read('contracts/v1/semantics/cases.json')).cases;
    for(const [id,fixture] of Object.entries(expectedFixtures)){
      const entry=cases.find(x=>x.id==='native.oresllm-'+id);
      assert.ok(entry,'native suite omits '+id);
      assert.equal(entry.kind,'native_exit');
      assert.equal(entry.expected_exit,42);
      assert.equal(entry.backends.llvm,'required');
      assert.equal(entry.backends.graal,'required');
      assert.equal(entry.source,'fixtures/oresllm-'+id+'.ores');
      assert.equal(read('contracts/v1/semantics/'+entry.source),fixture);
    }
  }else if(backend==='cranelift'){
    const rust=read('tests/oresllm_scalar_parity.rs');
    assert.match(rust,/CARGO_BIN_EXE_orescranelift/);
    assert.match(rust,/run_case\(expression, Some\(expected\)\)/);
    assert.match(rust,/run_case\(expression, None\)/);
    for(const x of POS){
      assert.ok(rust.includes('("'+x.expression+'", "'+x.expected_i64+'")'),
        'native Cranelift oracle missing '+x.id);
    }
    for(const x of [...OVER,...BAD]){
      assert.ok(rust.includes('"'+x.expression+'"'),'native Cranelift reject missing '+x.id);
    }
  }else if(backend==='net'){
    const cs=read('verification/Ores.Net.Tests/CrossBackendCheckedI64ParityTests.cs');
    assert.match(cs,/OresllmScalarIntersectionCases\(\);/);
    assert.match(cs,/OresIrReferenceEvaluator.Evaluate/);
    assert.match(cs,/CilEmitter.EmitLibrary/);
    assert.match(cs,/CilReferenceEvaluator.Evaluate/);
    for(const x of [...POS,...OVER,...BAD])
      assert.ok(cs.includes('"'+x.expression+'"'),'managed scalar case missing '+x.id);
  }else if(backend==='wasm'){
    const wasm=read('tests/oresllm-five-backend-native.test.mjs');
    assert.match(wasm,/ORESLLM_REQUIRE_REAL_WASM_PARITY/);
    assert.match(wasm,/WebAssembly.instantiate/);
    assert.match(wasm,/for\(const x of c.positive\)/);
    assert.match(wasm,/for\(const x of c.checked_overflow\)/);
    assert.match(wasm,/for\(const x of c.compile_reject\)/);
    assert.match(wasm,/t.skip\(/);
  }else if(backend==='conformance'){
    const gate=read('tools/oresllm-parity-admission.mjs');
    assert.match(gate,/candidate-only-shape-validated/);
    assert.match(gate,/runner_id/);
    assert.match(gate,/j.steps.length>0/);
    for(const [id,fixture] of Object.entries(expectedFixtures)){
      assert.equal(read('fixtures/native/oresllm-'+id+'.ores'),fixture);
    }
    assert.equal(read('fixtures/native/oresllm-reject-unknown.ores'),
      'pub fnc main(): int { return unknown_value; }\n');
    assert.equal(read('fixtures/native/oresllm-reject-caret-xor.ores'),
      'pub fnc main(): int { return 5 ^ 3; }\n');
  }
});
test('host binding check does not claim native, NativeAOT, GPU or full parity',()=>{
  const self=read('tests/oresllm-backend-binding.test.mjs');
  assert.match(self,/HOST-ONLY/);
  assert.match(self,/does not execute/);
});
