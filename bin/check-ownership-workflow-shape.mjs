#!/usr/bin/env node
// Structural preflight only. No compiler execution, guest receipt, or
// Actions runner admission may be inferred from this result.
import { readFileSync } from 'node:fs';
import { inspectOwnershipWorkflowShape } from '../scripts/ownership-ci-workflow-shape.mjs';

const source=readFileSync(
  new URL('../.github/workflows/atto-critical-ownership-llvm-parity.yml',import.meta.url),
  'utf8'
);
const result=inspectOwnershipWorkflowShape(source);
if(!result.structurallyValid || result.verified || result.guestExecutionVerified ||
   result.checkedNativeCTestCount!==11) {
  console.error('ATTO_OWNERSHIP_WORKFLOW_SHAPE=FAILED',
    JSON.stringify(result.errors));
  process.exitCode=1;
} else {
  console.log('ATTO_OWNERSHIP_WORKFLOW_SHAPE=PASS_STATIC_ONLY');
  console.log('ATTO_OWNERSHIP_NATIVE_CTESTS=11_REGISTERED_NOT_EXECUTED');
  console.log('ATTO_OWNERSHIP_GUEST_PARITY=NOT_EXECUTED');
}
