# LLVM functor parity v1 — source-first implementation and admission gates

Status as of 2026-11-11: **LLVM guest NOT_EXECUTED**. This branch adds an independent C++17 host-only type-shape preflight. It is NOT conformance to the 17 checked-in AttoLang guest fixtures, and does not generate functor machine code or establish zero-GC.

Reference source:
- Java/GraalVM guest draft: oreslang-source.java PR #2830, head faa149c6bcc7510a659c86499f614efdaffcc4e8.
- Shared conformance draft: oreslang-platform-conformance PR #689, head c6425d0daf734f9382f9b47e0ee7ff8666996ea4.
- Corpus: 2 .atto positives / 36 assertions / 15 compiler negatives, FNV1 and FNV2 exact stdout.
- LLVM parent main: 0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c.

## What this branch actually implements

The header src/main/cpp/oreslang/types/FunctorTypeRules.hpp is a pure value-type checker over recursive generic signatures, with bounded parsing, no dynamic runtime, and explicit error categories. It covers:
- map: preserves the outer Option<T> / Result<T,E> constructor; type-checks the callback input.
- map_err: changes exactly the Result error slot; checks its callback input.
- and_then: checks the callback's returned outer constructor and forbids changing nominal Result errors.
- flatten: removes exactly one matching Option or Result layer; requires identical Result error domains.
- transpose: swaps Option<Result<T,E>> and Result<Option<T>,E> without touching deeper generic layers.
- denied rebuilding affine payloads through shared/mutable loans or after a move (host policy, NOT proven actual loan tracking).
- bounded generic syntax and deterministic negative mutation categories.

tests/functor_type_rules_test.cpp is an independent **host-only** test; it is not a .atto fixture and does not invoke oreslang-llvmc. .github/workflows/functor-type-shape-host.yml attempts GCC C++17 execution and Clang ASan/UBSan on the exact PR head, with tracked-file blob attestations. **A workflow with no executed steps is NOT_EXECUTED**, even if a run object exists.

## Blocking native gaps confirmed by source inspection

- src/main/cpp/oreslang/frontend/Parser.cpp type_identifier() currently admits only scalar int/bool/String generic payloads. Nested Option<Result<...>> is rejected.
- src/main/cpp/oreslang/types/NativeSumTypes.hpp resolve_native_sums() rejects sum/class/array constructor payloads without an ownership/lifetime ABI. It only admits is_some, is_none, is_ok, is_err, unwrap, unwrap_or, and expect on native sum receivers.
- src/main/cpp/oreslang/backend/LlvmCodegen.cpp handles only scalar tagged sum construction/access. There is no lowering here for functor callbacks, closure environments, nested tagged payloads, or recursive destructor paths.
- These are intentional safety barriers. Do not bypass them by pretending a pointer or owned value is an untyped int or by interpreting the guest in a C++ test.

## Next required implementation slices

1. **Frontend / typed IR:** Extend type syntax and typed AST to retain recursively nested generic constructors, nominal errors, typed callback parameters and return types, and closure capture modes; resolve and reject arity/shape mismatches in the real compiler.
2. **Owned native sum ABI:** Define tagged nested payload layouts and one-owner transfer rules, including tag validity, layout checks, rollback on partial construction, deterministic destruction, and no implicit affine copies. Keep shared borrow, mutable borrow, and consumed states distinguishable.
3. **Native lowering:** Lower map, map_err, and_then, flatten, and both transposes to verified LLVM IR with lazy callback invocation and no callback on None/Err as appropriate. Preserve exact constructor and error domains, including nested F<G<H<T>>>.
4. **Real .atto compiler/guest gate:** Reuse the identical 17 fixtures from conformance PR #689, compare each checked-in blob SHA, require both exact stdout strings and compiler diagnostics matched to expected categories. Run O0/O2 and independently generated native binaries. Disallow pass-on-parse-error for positive fixtures.
5. **Ownership proof:** Add ASan/UBSan execution on guest binaries, explicit destructor counters for Ok/Err/None, consumed-after-move negatives, leak/double-free and allocation tests, including failed callbacks and nested short-circuit paths.
6. **Cross-backend rollout:** Only after LLVM guest evidence exists, extend the same immutable fixture corpus and contracts to .NET Native AOT, Wasm, Forge, and Cranelift. Keep each backend PENDING until its own compiler and execution evidence exists.

## CI provenance / private source

Record source PR head SHA, full or precisely scoped Git tree/blob inventory, runner head, test command, executed job steps, O0/O2 and sanitizer outcomes, native artifact hashes, and per-case diagnostics.

**No cross-org private source export is approved by this branch.** Oreslang conformance security incidents #22 and #77 remain open and explicitly require independently reviewed authorization bound to the exact destination repository ID/source snapshot. Available GitHub Actions minutes and private repository visibility alone are insufficient authorization. Prefer running the source-org workflow; if it does not allocate a runner, report NOT_EXECUTED and resolve the approval boundary before any alternate carrier.

## Merge gate

Keep this PR **draft**. A host-only shape-rule green run can support the standalone type algebra, not LLVM functor parity or zero-GC. No source-free model, static grep, parser-only negative, or no-step CI may upgrade the registry's LLVM PENDING status.
