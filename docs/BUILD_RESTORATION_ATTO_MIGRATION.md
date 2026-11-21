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

## Static inventory test procedure (2026-11-20/21)

The review-only build restoration contains a source-free checker and **10 positive/negative regression tests**, both verified locally from GitHub-blob-identical source:

```sh
python3 scripts/verify-production-cmake-inventory.py --root .
python3 -m unittest discover -s scripts -p 'test_verify_production_cmake_inventory.py' -v
```

Local synthetic suite result: `Ran 10 tests ... OK`, with checker Git blob `99c54b01e402f19553f0dd2b68495c4beab9ed50` and test suite blob `e4086ff788d0b9960624c20e6028ce787d381c48`. The local Python tests exercise synthetic fixtures, **not a clone of private LLVM source**; the checker was matched byte-for-byte by Git SHA to the committed file. Source-tree readback separately found 620 `add_test` invocations, 183 direct CMake path references and 637 project-local referenced source/test paths, all regular non-symlink Git blobs at the reviewed head.

This preflight checks structural completeness and fail-closed traversal. It cannot prove actual CMake configure/build, LLVM compiler behavior, host runtime, O0/O2 guest execution or sanitizer passes. Those production tests remain **NOT_EXECUTED** until an authorized source-side runner with nonzero steps is available. Exact PR head and independent reviewer approval are required before any merge.
