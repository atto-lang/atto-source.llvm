// Source-only fail-closed guard against malformed/duplicated ownership CI.
// Structural validation NEVER authorizes a runner, CTest or compiler guest.
const STEPS = [
  'Bind source to exact PR head and tree',
  'Install Clang/LLVM developer toolchain',
  'Build production LLVM compiler (not a mock)',
  'Execute production ownership CFG join metadata gate',
  'Execute seventeen source-owned place, identity and NLL integrity CTests',
  'Prove twenty scoped compiler CTest cases are registered',
  'Require compiler diagnostics and Clang-linked native guest',
  'Execute 100 real compiler fixtures in two matrices and fail-closed mutations',
  'Reverify checkout head and clean tracked source'
];
const NAMES = [
  'take_place_classification','share_place_authority','share_projection_root',
  'compiler_resolved_place_origin','identity_inventory_30','nll_partial_move_30',
  'nll_closed_world_16','derived_last_use_12','opaque_loan_metadata_10',
  'loan_write_domain_6','mir_receipts_30','mir_opaque_effects_18',
  'ir_ssa_store_census_30','ir_store_live_readback_14',
  'source_field_write_link_26','reinitialize_store_witness_48',
  'source_field_cfg_96'
];
const fail = code => ({
  structurallyValid:false, verified:false, guestExecutionVerified:false,
  status:'OWNERSHIP_WORKFLOW_STATIC_CHECK_FAILED',errors:[code]
});
const once = (s,fragment) => s.split(fragment).length === 2;
const finalMarker = `          echo "ATTO_LLVM_FINAL_TREE=$(git rev-parse 'HEAD^{tree}')"`;

export function inspectOwnershipWorkflowShape(source) {
  if(typeof source !== 'string' || !source || source.length>100000 ||
     source.includes('\r') || source.includes('\0'))
    return fail('INVALID_WORKFLOW_BYTES');
  const lines=source.trimEnd().split('\n');
  if(lines.filter(x=>x==='jobs:').length!==1 ||
     lines.filter(x=>x==='  llvm-native-critical-share:').length!==1)
    return fail('DUPLICATE_OR_MISSING_WORKFLOW_JOB');
  if(!once(source,'\n  pull_request:\n') ||
     !source.includes("      - 'audit/llvm-typed-mir-receipt-fence-*'") ||
     !once(source,"\n      - 'audit/llvm-source-physical-fieldwrite-link-*'") ||
     !once(source,"\n      - 'audit/llvm-restore-witness-to-store-*'") ||
     !once(source,"\n      - 'audit/llvm-source-restoration-cfg-dom-v1-*'") ||
     !once(source,"\n      - 'audit/llvm-source-cfg-50-guests-and-regressions-*'") ||
     !once(source,"\n      - 'tests/contracts/atto_ownership_source_cfg_50.json'") ||
     !once(source,"\n      - 'tests/ownership_source_cfg_compiler_matrix.py'") ||
     !once(source,"\n      - 'tests/test_ownership_source_cfg_compiler_matrix.py'") ||
     !once(source,"\n      - 'tests/native_source_field_cfg_96_test.cpp'") ||
     !once(source,"\n      - 'src/main/cpp/oreslang/types/OwnershipSourceFieldFlow.hpp'") ||
     !once(source,"\n      - 'tests/native_reinitialize_store_witness_48_test.cpp'") ||
     !once(source,"\n      - 'src/main/cpp/oreslang/backend/NativeSourceFieldWriteLinks.hpp'"))
    return fail('MISSING_EXACT_HEAD_TRIGGER');
  const defined=lines.filter(line => line.startsWith('      - name: '))
    .map(line=>line.slice('      - name: '.length));
  if(defined.length!==STEPS.length || STEPS.some((x,i)=>defined[i]!==x))
    return fail('MISSING_OR_REORDERED_WORKFLOW_STEP');
  if(lines.filter(line=>line.startsWith('      - uses: ')).length!==1 ||
     !source.includes('actions/checkout@11d5960a326750d5838078e36cf38b85af677262'))
    return fail('MISSING_PINNED_SOURCE_CHECKOUT');

  // YAML literal run blocks are indented by ten spaces. In particular an
  // unindented stray test or duplicate tail MUST NOT become YAML structure.
  let inRun=false;
  for(const line of lines) {
    if(line.startsWith('      - ')) { inRun=false; continue; }
    if(line==='        run: |') { inRun=true; continue; }
    if(inRun && line.trim() && !line.startsWith('          '))
      return fail('ORPHANED_OR_UNINDENTED_RUN_COMMAND');
  }
  if(lines.at(-1)!==finalMarker ||
     !once(source,finalMarker) ||
     !once(source,'      - name: Reverify checkout head and clean tracked source'))
    return fail('TRAILING_OR_DUPLICATE_FINAL_RECEIPT');
  if(!once(source,"int(root.get('tests', '-1')) != 17") ||
     source.includes("!= 16 or") || source.includes("!= 15 or") || source.includes("!= 14 or") || source.includes("!= 13 or") ||
     source.includes("!= 12 or") ||
     source.includes("!= 11 or") || source.includes("!= 9 or"))
    return fail('STALE_NATIVE_CTEST_CARDINALITY');
  for(const name of NAMES) {
    const pattern=`          grep -Eq 'Test +#[0-9]+: native_ownership_${name}$' /tmp/atto-place-inventory.txt`;
    if(!once(source,pattern)) return fail('MISSING_OR_DUPLICATED_CTEST:'+name);
    if(!source.includes(name+'|') && !source.includes(name+')'))
      return fail('MISSING_NATIVE_CTEST_REGEX:'+name);
  }
  if(!source.includes('oreslang-native-loan-write-domain-6-test') ||
     !source.includes('oreslang-native-mir-receipts-30-test') ||
     !source.includes('oreslang-native-mir-opaque-effects-18-test') ||
     !source.includes('oreslang-native-ir-ssa-census-30-test') ||
     !source.includes('oreslang-native-ir-store-live-readback-14-test') ||
     !source.includes('oreslang-native-source-field-write-link-26-test') ||
     !source.includes('oreslang-native-reinitialize-store-witness-48-test') ||
     !source.includes('oreslang-native-source-field-cfg-96-test') ||
     !source.includes('--output-junit /tmp/atto-ownership-place.xml'))
    return fail('MISSING_NATIVE_BUILD_OR_RECEIPT');
  if(!source.includes('ATTO_OWNERSHIP_REAL_COMPILER_FIXTURES=50/50_EXECUTED') ||
     !source.includes("root.get('tests', '-1')") ||
     !source.includes('atto_ownership_place_mir_real_compiler_50') ||
     !source.includes('--output-junit /tmp/atto-real-compiler-50.xml'))
    return fail('MISSING_REAL_COMPILER_MATRIX_RECEIPT');
  if(!once(source,'ATTO_SOURCE_CFG_REAL_COMPILER_FIXTURES=50/50_EXECUTED') ||
     !once(source,'ATTO_LLVM_SOURCE_CFG_GUEST=50/50_REAL_COMPILER_EXECUTED') ||
     !once(source,'--output-junit /tmp/atto-source-cfg-50.xml') ||
     !once(source,"          grep -Eq 'Test +#[0-9]+: native_ownership_source_cfg_manifest_18_mutations$' /tmp/atto-source-cfg-fixture-inventory.txt") ||
     !source.includes('atto_ownership_source_cfg_real_compiler_50') ||
     !source.includes("observed != ['atto_ownership_source_cfg_real_compiler_50']") ||
     !source.includes("int(root.get('skipped', '1'))") ||
     !source.includes("int(root.get('failures', '1'))"))
    return fail('MISSING_SOURCE_CFG_REAL_GUEST_EVIDENCE');
  if(!source.includes('test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"') ||
     !source.includes('git diff --cached --quiet --exit-code'))
    return fail('MISSING_EXACT_HEAD_CHECK');
  return {
    structurallyValid:true, verified:false, guestExecutionVerified:false,
    status:'OWNERSHIP_WORKFLOW_SHAPE_ONLY_NOT_EXECUTION',
    checkedNativeCTestCount:NAMES.length,errors:[]
  };
}
