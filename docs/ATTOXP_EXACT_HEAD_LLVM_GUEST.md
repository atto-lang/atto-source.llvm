# Real source-local AttoXP LLVM scalar admission receipt

This workflow builds the real oreslang-llvmc production compiler from the
exact PR head and tree in the LLVM source repository. It executes nine
previously registered CTest AttoXP counter/shared scalar checks through
compiled LLVM IR, Clang linking and native exit-code validation. It also
builds the same .atto counter guest at O0 and O2 and checks exit status 42,
empty output and compiler, LLVM IR, and binary digests.

This is NOT a compiler copy, a restricted translator, the AttoXP native UI
ABI, or UIKit/Android rendering. No private sources/artifacts cross orgs.
Source hash checks are HOST_ONLY and cannot substitute for native guest runs.

The workflow proves its narrower claims only when the specific run has
nonzero executed steps, exact head/tree and all nine named tests. A failed
or empty-step workflow means NOT_EXECUTED, not compiler correctness.
It does not supersede draft PR #1353 or enable AttoXP compiler enrollment
without independently approved binary provenance.
