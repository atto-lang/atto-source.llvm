# AttoLang LLVM exact IR snapshot readback — v1

**2026-11-07; draft; bounded host LLVM IR evidence, NOT typed loan completeness or compiled guest parity.**

## Previously observed weakness

The existing `NativeIrUseStoreCensus` traversed all real LLVM instructions and reconciled the aggregate reciprocal SSA use counts; however, its post-emission `contains_store` and `CompilerFieldWriteLinks::inspect` checked only store identity, operands, store order, and **total instruction count**. A later mutation to a non-store call operand, PHI incoming operand, branch successor, instruction metadata or function attributes could preserve store identity and cardinality. The old source-restoration-to-store receipt could then be accepted despite a changed control-flow graph or use-def edge.

This is a correctness and audit-evidence gap even if the actual LLVM module remains verifier-valid.

## Implementation

- `NativeIrOwnershipCensus.hpp` introduces `BoundedNativeIrText`, an unbuffered `llvm::raw_ostream` that captures the LLVM module's actual printed IR bytes, **capped at 64 MiB**. This is exact byte equality, **not** a short/checksummed hash or caller-supplied metadata. If the cap is exceeded, the census fails closed with an ownership violation before storing an unbounded receipt.
- `collect_native_ir_ssa_census(module)` continues to derive store addresses, ordinal, SSA counts and reciprocal uses from live LLVM instructions, and also retains the bounded exact IR text. The old census is bound to its original module lifetime and must not be reused after the module is destroyed or concurrently mutated.
- `NativeIrUseStoreCensus::matches_current_module()` prints the currently live module afresh and requires byte-for-byte equality. It rejects null/uninitialized receipts, mismatches and printing failures. Neither this method nor `contains_store` dereferences cached erased LLVM instruction pointers.
- `contains_store` now rejects **any** changed IR text before its existing physical store walk. `CompilerFieldWriteLinks::inspect` also explicitly requires a live exact-module snapshot match, denying stale restoration tickets even if the actual `StoreInst` itself is unchanged.
- No previous method sets `ownership_complete`, `source_ownership_complete` or `guest_verified` to true, and no optimized IR, source LoanId inventory, lexical borrow or SSA lifetime is inferred from byte-for-byte module equality.

## Native mutation suite and CI gate

New `tests/native_whole_module_ir_snapshot_58_test.cpp` constructs a **real verifier-valid LLVM module** containing ten distinct branch diamonds and ten PHI nodes, with source field store, loads, calls, native SSA values and real branch successors. There are **8 positive host LLVM baseline assertions** followed by **50 independent mutation probes** at ten sites each:

1. Change a non-store call argument SSA value while keeping the source store.
2. Change a PHI incoming value at each distinct merge.
3. Swap the two valid successor edges for each distinct conditional branch.
4. Change metadata on each of ten different load instructions.
5. Change function attributes with ten distinct keys.

For each mutation, the test validates LLVM verifier acceptance, checks that old exact module snapshots and old store receipts fail, then restores the IR and checks exact equality again. All are **authored native host LLVM tests, NOT_EXECUTED** until real LLVM development libraries and runner admission are available.

CMake adds `native_ownership_whole_module_ir_snapshot_58` as the **18th** native ownership CTest with `-Wall -Wextra -Wpedantic -Werror`. The source-org workflow requires exactly eighteen nonfailed/nonskipped named native CTests and full JUnit, plus the existing twenty scoped compiler checks and two distinct fifty-case LLVM/Clang `.atto` guest matrices. Its source-only integrity mutation suite expands to `ownership_ci_workflow_shape_58.test.mjs` with five new removal/duplication/bypass probes.

**Source-only structural evidence:** the fetched 58-case CI integrity script passed **58/58 in isolated V8 using a lightweight Node test/assert shim**, reporting `checkedNativeCTestCount=18`, `verified=false` and `guestExecutionVerified=false`. This is not actual hosted Node, C++ test execution, compiler-backed guest proof or sanitizer evidence.

## Explicit proof boundaries

An IR text snapshot checks **current representation equivalence**, not **temporal optimizer generations**. If an optimizer changes a module and then restores byte-identical IR, this checker cannot see that a mutation occurred. Full generation fencing still requires compiler/pass-manager instrumentation that invalidates per-pass provenance (or a stable typed SSA/source origin mapping with post-pass proof). No compiler-owned complete loan-use inventory or provenance of untracked lexical borrows is established here. No new AttoLang ownership feature is enabled by this patch. It does not prove actor suspension, cancellation, exceptional cleanup or five-backend parity.

Release hold: GitHub Actions source-org runner admission still has `steps=null` and the previously documented payment/billing/spending-limit blocker ([LLVM issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159)). Only an authorized billing owner can resolve that account policy. Require same-org source-free nonzero-step canary, exact-head hosted Node shape and fixture checks, 18 real native CTests with sanitizers, 20 scoped compiler cases, and both 50-source actual compiler/Clang suites with detailed receipts. Keep all stacked PRs draft until eligible independent ORESoftware review and immutable-head verification. Do not mirror private source across orgs.
