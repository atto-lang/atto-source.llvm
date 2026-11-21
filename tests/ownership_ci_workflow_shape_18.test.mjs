import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { inspectOwnershipWorkflowShape as inspect }
  from '../scripts/ownership-ci-workflow-shape.mjs';

const workflow=readFileSync(
  new URL('../.github/workflows/atto-critical-ownership-llvm-parity.yml',import.meta.url),
  'utf8'
);
const denied=(source,code)=>{
  const r=inspect(source);
  assert.equal(r.structurallyValid,false);
  assert.equal(r.verified,false);
  assert.equal(r.guestExecutionVerified,false);
  assert.ok(r.errors.some(x=>x.startsWith(code)),JSON.stringify(r.errors));
};
test('01 canonical twelve-CTest workflow has valid source-only shape',()=>{
  const r=inspect(workflow);
  assert.equal(r.structurallyValid,true,JSON.stringify(r.errors));
  assert.equal(r.checkedNativeCTestCount,12);
  assert.equal(r.verified,false);
  assert.equal(r.guestExecutionVerified,false);
});
test('02 duplicated final workflow step cannot survive',()=>{
  denied(workflow+'      - name: Reverify checkout head and clean tracked source\n',
    'MISSING_OR_REORDERED_WORKFLOW_STEP');
});
test('03 stray command after final receipt is rejected',()=>{
  denied(workflow+' /tmp/atto-place-inventory.txt\n',
    'ORPHANED_OR_UNINDENTED_RUN_COMMAND');
});
test('04 stale eleven-test count is rejected',()=>{
  denied(workflow.replace("int(root.get('tests', '-1')) != 12",
      "int(root.get('tests', '-1')) != 11"),'STALE_NATIVE_CTEST_CARDINALITY');
});
test('05 missing typed-MIR CTest inventory entry is rejected',()=>{
  denied(workflow.replace(
      "          grep -Eq 'Test +#[0-9]+: native_ownership_mir_receipts_30$' /tmp/atto-place-inventory.txt\n",
      ''),'MISSING_OR_DUPLICATED_CTEST:mir_receipts_30');
});
test('06 duplicated CTest inventory assertion is rejected',()=>{
  const line="          grep -Eq 'Test +#[0-9]+: native_ownership_mir_receipts_30$' /tmp/atto-place-inventory.txt\n";
  denied(workflow.replace(line,()=>line+line),
    'MISSING_OR_DUPLICATED_CTEST:mir_receipts_30');
});
test('07 50 actual LLVM compiler fixtures must remain required',()=>{
  denied(workflow.replace('ATTO_OWNERSHIP_REAL_COMPILER_FIXTURES=50/50_EXECUTED',
    'ATTO_OWNERSHIP_REAL_COMPILER_FIXTURES=SHAPE_ONLY'),
    'MISSING_REAL_COMPILER_MATRIX_RECEIPT');
});
test('08 source PR head trigger must remain present',()=>{
  denied(workflow.replace("      - 'audit/llvm-typed-mir-receipt-fence-*'",
    "      - 'unused-branch-*'"),'MISSING_EXACT_HEAD_TRIGGER');
});
test('09 duplicate job declaration is rejected',()=>{
  denied(workflow.replace('\njobs:\n','\njobs:\njobs:\n'),
    'DUPLICATE_OR_MISSING_WORKFLOW_JOB');
});
test('10 the final exact-head receipt cannot be suppressed',()=>{
  denied(workflow.replace(
    '          echo "ATTO_LLVM_FINAL_TREE=$(git rev-parse \'HEAD^{tree}\')"',
    '          echo "FAKE_FINAL_RECEIPT"'),
    'TRAILING_OR_DUPLICATE_FINAL_RECEIPT');
});
test('11 all job steps must remain unique and correctly ordered',()=>{
  denied(workflow.replace('      - name: Build production LLVM compiler (not a mock)',
    '      - name: Install Clang/LLVM developer toolchain'),
    'MISSING_OR_REORDERED_WORKFLOW_STEP');
});
test('12 checkout must remain pinned to vetted immutable action',()=>{
  denied(workflow.replace('actions/checkout@11d5960a326750d5838078e36cf38b85af677262',
    'actions/checkout@v4'),'MISSING_PINNED_SOURCE_CHECKOUT');
});
test('13 malformed line endings are rejected before parsing',()=>{
  denied(workflow.replace('\n','\r\n'),'INVALID_WORKFLOW_BYTES');
});
test('14 untrusted empty source never claims workflow execution',()=>{
  denied('','INVALID_WORKFLOW_BYTES');
});
test('15 missing twelfth opaque-effects CTest inventory is rejected',()=>{
  denied(workflow.replace(
    "          grep -Eq 'Test +#[0-9]+: native_ownership_mir_opaque_effects_18$' /tmp/atto-place-inventory.txt\n",
    ''),'MISSING_OR_DUPLICATED_CTEST:mir_opaque_effects_18');
});
test('16 duplicated opaque-effects inventory assertion is rejected',()=>{
  const line="          grep -Eq 'Test +#[0-9]+: native_ownership_mir_opaque_effects_18$' /tmp/atto-place-inventory.txt\n";
  denied(workflow.replace(line,()=>line+line),
    'MISSING_OR_DUPLICATED_CTEST:mir_opaque_effects_18');
});
test('17 missing native opaque-effects build target is rejected',()=>{
  denied(workflow.replace(
    'oreslang-native-mir-opaque-effects-18-test',
    'dummy-native-test'),'MISSING_NATIVE_BUILD_OR_RECEIPT');
});
test('18 incomplete source compiler matrix invocation is rejected',()=>{
  denied(workflow.replace(
    "--output-junit /tmp/atto-real-compiler-50.xml",
    "--output-junit /tmp/fake.xml"),
    'MISSING_REAL_COMPILER_MATRIX_RECEIPT');
});
