# Restore production LLVM CMake after Atto migration

**Draft. Full native build and CI have NOT yet been executed.**

## Immutable inputs

- Atto source main SHA: fe4f253b5932f8f00ab4a20f9b9bc2019d17553b
- Original Atto carrier-only CMake blob: 64859038b16af8c42dd102ed9f5db5b95736ddb9
- Archived full CMake source: ores-truffle-oreslang/oreslang-source.llvm main blob fedd2918a86d2688d1859c5458bbf483c240f6c5
- Restored CMake is archived full source plus current Atto extra targets: atto-checked-actor-ir, oreslang-actor-slab-heap-test, oreslang-actor-slab-heap-bench.
- All 183 unique literal current-source-directory file references found in the archived CMake resolve in the Atto tree. Static inventory only; not compilation.

## Why

Current main's 3,062-byte root CMakeLists.txt identifies itself as a temporary carrier-only reconstruction and omits the production targets and large CTest registry. Production source CMake is 600,337 bytes. The repair preserves newer Atto actor-slab test and checked-actor IR targets.

## Required verification before promotion

1. Recheck exact PR head and Git root-tree/blob/mode provenance. Reject partial trees or symlink substitution.
2. Configure full CMake against LLVM18+ and available dependencies. Build oreslang-llvmc, atto-checked-actor-ir and both slab test/bench targets. Missing targets fail closed.
3. Execute full CTest inventory and focused scalar, actor, ownership, borrow and sanitizer tests. Record real runner steps and counts, O0/O2 compiled guest results and negative no-IR cases.
4. Resolve private CI source identity migration and obtain independent source/carrier security approvals before any private source export. Do not infer Actions budget from zero-step failure.
5. Reconcile #813/#814/#815 on this full tree, with source and compiler digest checks.

## Scope limitations

This draft restores only the build definition; it is NOT evidence of successful CMake generation, native LLVM compilation, complete guest execution, CI admission, sanitizer coverage or authorized source mirroring. Keep DRAFT until independently reviewed full-tree CI evidence exists.
