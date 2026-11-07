# LLVM source-field CFG: 50 additional compiler/guest candidates and fail-closed audit

**November 7, 2026 — DRAFT / guest NOT_EXECUTED / requires Actions admission.**

## Audit finding and direct repair

The source-field restoration CFG in parent [#1547](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1547) records actual AST-derived move, read and restoration effects, but `OwnershipSourceFieldFlowBuilder` previously returned without emitting any CFG operation for three forms of **untracked** class-field operations:

- `Expression::Kind::Field` when its named receiver was not in the tracked class-root set
- `Expression::Kind::Take` of such a field
- `Statement::Kind::FieldWrite` to an untracked root with no restoration ticket

The new code **rejects all three as unclassified ownership effects** whenever the source-flow proof is active, rather than silently treating them as inert. This narrows admission and does not grant new NLL, SSA, or guest authority. It is intentionally conservative for non-class constructs that share field-like syntax.

## 50 distinct new `.atto` source programs

`tests/contracts/atto_ownership_source_cfg_50.json` defines **exactly fifty** unique complete AttoLang source strings in five semantic groups, ten per group:

| Group | Oracle | Coverage |
| --- | --- | --- |
| restored-direct | 10 real Clang-linked guest exits | `rt take p.left`, direct scalar restore, post-restore read |
| restored-both-branches | 10 real Clang-linked guest exits | Both `if` predecessors restore a moved class field, then read at join |
| restored-sibling | 10 real Clang-linked guest exits | Move + restore one field, then read two independent class fields |
| unrestored-one-branch | 10 required checker diagnostics | A field is restored only on one predecessor; fail ownership join |
| unrestored-read | 10 required checker diagnostics | Read class field after move with no restoration |

These are **candidate compiler programs**, not a pass result or executed test. The negative one-branch cases are expected to be caught by the existing affine-state predecessor equality gate; compiling them alone does NOT prove the newer fixed-point verifier ran. The real field-flow C++ tests in the parent continue to test that separate verifier.

The new `tests/ownership_source_cfg_compiler_matrix.py` calls the repository's existing `tests/contract.py` on each source with the actual `oreslang-llvmc`, with required semantic diagnostic for negatives and LLVM IR + real Clang link + exact native process exit for positives. It rejects absent binaries, duplicate IDs/source bytes, incorrectly categorized/missing proof forms, invalid exit or diagnostic, silent skipped cases, timeouts, or failed guest/link/compiler. It emits `ATTO_SOURCE_CFG_REAL_COMPILER_FIXTURES=50/50_EXECUTED` only **after** all fifty subprocess contracts succeed.

`tests/test_ownership_source_cfg_compiler_matrix.py` adds **18 host-only manifest corruption tests** (counts, duplicate programs, unsupported statuses, disabled compiler/Clang gates, comment/shape tampering, wrong one-vs-both-branch results, path traversal, invalid diagnostic and missing guest oracle). The CMake test targets are:

- `native_ownership_source_cfg_manifest_18_mutations` — host-only integrity, 18 tests
- `atto_ownership_source_cfg_real_compiler_50` — real compiler, diagnostic, linked guest matrix

These are independent of the existing `native_ownership_place_mir_manifest_19_mutations` and `atto_ownership_place_mir_real_compiler_50`: **100 total distinct compiler inputs** are now mandatory across the two matrices, in addition to the 17 focused native ownership CTests and 20 scoped compiler checks.

## Workflow and exact evidence requirements

The same source-org `atto-critical-ownership-llvm-parity.yml` workflow requires both new CTest identities, runs the 18 manifest corruption checks and 50 real compiler programs, parses separate **nonfailed, nonskipped, exact-name JUnit receipts**, and requires a distinct terminal marker from the second real compiler matrix. The previous 50-program matrix remains intact. Any fixture failure, semantic mismatch, missing executable IR, missing Clang or absent process-exit receipt fails the job. All paths/branch triggers include the new sources and the workflow. New source-only integrity suite `tests/ownership_ci_workflow_shape_46.test.mjs` has 46 negative/positive tests to detect missing, duplicated, or forged workflow gates.

**Verified on fetched exact-source bytes in isolated V8 with a Node test/assertion shim: 46/46 source-only workflow integrity groups passed, `checkedNativeCTestCount=17`, `verified=false`, `guestExecutionVerified=false`.** An independent source-file inventory found fifty unique IDs/source strings and correct 30 positive/20 negative category counts, and identified both new CMake tests. This is not a hosted Node 22 test nor execution of Python, C++, LLVM, Clang, or the 100 guest programs.

## Release hold

Source-org GitHub Actions has repeatedly failed **before runner admission** (`steps=null`); the GitHub provider annotation tracked on infrastructure issue #159 and conformance issue #315 reports failed recent payments **or** an Actions spending limit needing adjustment. Only an authorized billing administrator can diagnose and resolve it. First require a source-free same-org canary with a real runner and nonzero executed steps, then rerun this exact head through hosted 46 Node tests, 17 native ownership CTests with ASan/UBSan, 20 scoped compiler tests, **two independently authenticated 50-program real-compiler matrices**, and negative mutations. Capture immutable SHA/tree, JUnit, compiler digest, Clang link and guest receipts.

The source CFG does not prove complete typed SSA loan-use provenance, optimized LLVM instruction dominance/generation fencing, exception/actor/cancellation/suspension safety or five-backend parity. These remain nonpromoting `false` flags. Keep PR DRAFT, require independent eligible reviewer and do not mirror private code across organizations or loosen branch/security gates.
