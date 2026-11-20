# LLVM trap-to-Result reconciliation (restricted native AOT)

## Decision

`trap expression` is **Result-valued**, never an implicit `Option` conversion:

* A verified, nonthrowing `int` or `bool` scalar yields exactly `Ok(value): Result<T,Error>`.
* If the operand is **already** a typed `Result<T,E>`, `trap` preserves that value, its `Ok`/`Err` tag, its error type `E`, and one evaluation. It does **not** construct `Result<Result<T,E>,Error>`.
* An `Option<T>` operand or target is rejected until an explicit error mapping/contract exists. `None` is not a representation of a trapped error.
* Checked integer overflow under this restricted `trap` remains a terminal LLVM trap, never a fabricated `Err`; panic, raise, cancellation and host faults are not intercepted.

### Native executable subset and fail-closed boundaries

The legacy LLVM native sum ABI uses a bounded, generation-checked tagged record. The new compiler-owned `Result<T,Error>` descriptor currently admits **only the Ok side minted by `trap`**, for checked integer literals / additive, subtractive and multiplicative scalar trees or a typed `bool` / `int` local. The Error payload is opaque; native `Err(error)` construction, explicit `Ok(...): Result<T,Error>`, String allocation under `trap`, class/actor ownership, arbitrary calls, throw/recover, async and cancellation capture remain rejected **before emitting IR**.

A previously constructed `Result<T,E>` local (including `Err`) may be returned unchanged through `trap`. This is *typed value idempotence*, not general exception capture during evaluation of a throwing expression.

```atto
val Result<int,Error> fresh = trap (40 + 2);       // Ok(42)
val Result<int,int> old = Err(7);
val Result<int,int> same = trap old;                 // Err(7), not Ok(Err(7))
```

The compiler does not claim full `Error` class lineage, throw-capture cleanup, actor supervisor CONTROL delivery, or broader backend parity. Future work must implement typed exception edges and verified Error/ownership cleanup *before* admitting effectful `trap` operands. Do not silently replace `Error` with an i64 user payload.

## Provenance / overlap disposition

* Archived LLVM [#1774](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1774) introduced a restricted pure-integer `Option<int>` trap, now superseded.
* Archived LLVM [#1879](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1879) migrated pure integer trap to `Result<int,Error>`, with terminal overflow; it explicitly left an already-Result operand unimplemented. **Incorporated** this behavior and extended the existing-Result idempotence rule.
* Archived LLVM [#1886](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1886) documents why the older Assert/`trap -> None` corpus must not define modern semantics.
* Atto LLVM [#63](https://github.com/atto-lang/atto-source.llvm/pull/63) provides a distinct host-only checked-parse Result C ABI, **independent** of this compiler's preexisting native tagged-sum record runtime. This PR does not replace or claim integration with that ABI.
* Follow `atto-spec` and conformance as the normative authority for any widening beyond the restricted subset.

## Verification expectations

`ctest --test-dir build --output-on-failure -R '^atto_trap_'` registers 18 tests:
10 real source→compiler→LLVM→Clang→native guest exits at -O0/-O2, 7 negatives requiring a real nonzero compiler diagnostic and empty stdout/IR, and 1 terminal-overflow guest at both optimization levels requiring a real SIGILL/SIGTRAP. Each source fixture is pinned to its exact Git blob SHA, verified by the test harness.

This is a **test specification** until the test runner has actually executed those cases on the exact PR head. A zero-step workflow, source inspection or host-only static assertion must never be presented as guest proof.
