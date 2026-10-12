# Wasm32 multi-field class exact-source validation

## Verified merge (October 11, 2026 UTC)

- [Merged LLVM PR #140](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/140): full current main + Wasm32 structured class admission.
- [Successful private GitHub Actions run](https://github.com/zed-pkg-test/java-app/actions/runs/38114329434).
- Upstream tested source commit: `e88f85f74545cc4ffe383c5e6315b94726ed78b1`.
- **Exact upstream Git tree**: `917b4dbdca5fbc3d8fe4c282f23399f108644d72` (265 files). Workflow asserts `git rev-parse HEAD:mirror/llvm` equals this tree; it does not merely match selected blobs.
- Full CMake configured, every target built, **151/151 CTests passed**, including `wasm32_source_class_ownership_runtime`.
- Shared guest source execution produced **131/131** expected observations.
- Four native actor/lifecycle/CONTROL CTests each passed 20 repeated iterations.
- Checked concrete Wasm32 class fixture execution: four classes/borrow/mutation/guard programs returned `42n`; five invalid/unsupported cases were rejected without emitting IR.
- [Companion Wasm guest fixtures](https://github.com/ores-truffle-oreslang/oreslang-wasm/pull/2) compiled real source with fixed guest memory and digest-verified manifests.
- [Shared pinned source conformance cases](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/4) explicitly mark Java/GraalVM structured-class parity as pending.

## Scope limitations

This proves a bounded class layout with i64 fields, call-scoped loans and source
mutation under the experimental wasm32 core-Wasm guest profile. It **does not**
establish a host-visible structured-value ABI, actor lowering to wasm32,
continuation suspension, compiler-inferred capability admission, native heap
regions in wasm32, full Java/GraalVM parity, or production-grade untrusted
execution isolation. Source-level native actor/heap operations fail closed for
this wasm32 target rather than linking unsupported host runtime symbols.

The private CI mirror is an ephemeral build witness, not an authoritative code
distribution. The canonical implementation lives in LLVM `main`.
