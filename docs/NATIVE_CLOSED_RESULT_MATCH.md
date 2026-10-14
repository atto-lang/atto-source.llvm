# Closed native Result local and exhaustive match (stage 2, draft)

This *stacked* LLVM compiler experiment accepts exactly one immutable,
nominal `Result<void,ArrayError>` local initialized by a **known private,
zero-argument, closed Result constructor function**, then consumes it in a
single exhaustive `do match` with two terminal integer return branches.

```oreslang
fnc make(): Result<void, ArrayError> {
    return Err(ArrayError.Limit);
}

fnc report(): int {
    const Result<void, ArrayError> outcome = make();
    do match outcome over
        on Ok() -> { return 0; }
        on Err(error) -> { return 1; }
    end
}
```

This grammar is intentionally small and does NOT constitute support for
general Result arguments, generic Result, array try_add, borrowed Results,
captures, loops, exceptions, effects, actors, nonterminal arms, side effects
in match arms, wildcard arms, C ABI, or Wasm output.

## Admission and proof

The compiler enforces these invariants before emitting any LLVM IR:

1. Producer exists in the **closed nominal constructor** table, not an
   integer overload, class, future, module import, actor or unknown method.
   Forward producer definitions are allowed within one translation unit.
2. Consumer is **private**, has no parameters/effects, and returns `int`;
   at most 64 producers and 64 consumers per translation unit.
3. The only local is typed `const Result<void,ArrayError>`, assigned once
   from exactly one zero-argument private producer call.
4. `do match <same-local> over` has exactly two arms: one `Ok()` and
   one `Err(<local-error-binding>)`, in either order. No guards, default,
   aliases, copies, early returns, suspension or escaping references.
5. Each arm returns one compile-time integer. Both arms terminate, so
   every valid incoming value discharges its unique handling obligation.
   Missing, duplicate, extra, wildcard, altered subject, or nonterminal
   arms reject source before LLVM IR emission.
6. LLVM verifies the nominal result aggregate and the internal producer
   signature independently; it allocates a typed local, stores/loads the
   aggregate once, checks discriminant and nominal error tags, and branches
   to `Ok` or `Err`. Invalid tag/payload combinations trap, never
   alias a recoverable `Err` or `Ok`.
7. The experimental scalar/AOT fast path and wasm32 backend reject these
   consumers rather than emitting incomplete/unsound modules.

The stage-2 invariant is a simple finite-state proof:

`Pending -> Consumed -> (OkReturned | ErrReturned)`

Every legal source path goes through `Consumed` exactly once. Because the
source subset admits no loops, aliasing, exceptions or suspension, the AST
checks enumerate all possible control-flow terminal cases. **This is not
a proof for general CFGs**; those require ownership and lifetime analysis.

## Native conformance gate

CTest `native_result_closed_match_execution` runs the actual compiler
against three Result producers and three consumers (including reversed
arm order), checks typed LLVM IR and trap branches, and invokes a same-module
LLVM IR caller under Clang `-O0` and `-O2`. Negative tests assert specific
diagnostics and **zero emitted IR** for missing/duplicate match arms,
unknown/scalar producers, unhandled Results, public/effectful consumers,
symbol collisions, wrong local, and nonterminal control flow.

The same LLVM module is required because constructors and consumers use
`internal` linkage. This does **not** certify C/FFI or public ABI.

Before merge, run exact-head `oreslang-llvmc` build, native CTest suite,
ASan/UBSan, negative diagnostic matrix, Java/GraalVM conformance.
GitHub Actions jobs failing with zero steps are an infrastructure blocker
and **do not establish that any source-compiler tests passed**.
