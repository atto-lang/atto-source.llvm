# Critical handle_state and using — shared Java/Graal and LLVM language contract

Status: **static/lower-level parity in progress; NOT executable guest actor-source
parity**. Both compilers must eventually accept the same source programs, infer
the same callback effects and enforce the same scheduler/borrow rules.

## Selected language contract

```ores
define actor Counter as
    state { count: int = 0; }

    pub increment(): int using state.read, state.write {
        return self.handle_state(critical |s| -> {
            s.count = s.count + 1;
            return s.count;
        });
    }

    pub read(): int using state.read {
        return self.handle_state(critical |s| -> s.count);
    }
end
```

- `handle_state` is the **sealed, runtime-defined, intrinsically critical**
  function. User code may NOT define or override it. No redundant
  `critical self.handle_state(...)` prefix is required; the *intrinsic*
  itself is already marked critical in the compiler's function/effect metadata.
- The sole argument **MUST** be an **inline** `critical |state| -> ...`
  with exactly one state-access parameter,
  lexical actor affinity and no capability escape. Unmarked `|s| -> ...`
  MUST fail compilation. An unrelated `critical` lambda does not thereby
  acquire actor-state authority.
- `using` is a **reserved signature keyword**, not a reusable variable.
  It declares an **upper bound** on the effects performed by the function,
  including the invoked critical callback. A read callback requires
  `state.read` only; a write requires `state.write` (and `state.read`
  when it reads). Do not automatically add state.write for read-only
  callbacks. `using` does not grant a capability.
- The callback inherits the enclosing effect-row upper bound, and its
  transitive effects must be checked; the runtime intrinsic is
  *effect-polymorphic*. The compiler must preserve the distinction between
  `throw`, `raise`, `panic`, `io`, `suspend`, `block`, `ffi` and
  `alloc`. Unverified external calls must fail closed under an explicit
  `using` bound.
- `critical` is a scheduler-owned logical single-flight **admission
  property**, **not** a native mutex held across I/O. Same-continuation
  synchronous nested critical callbacks are permitted; an inner scoped
  state loan fences the outer loan while active.
- The callback may perform I/O, `await`, `select`, or an explicitly
  declared blocking call, **if** its `using` row allows the corresponding
  effects and the target profile admits the operation. A suspension releases
  the logical lane. State-backed mutable borrows are retired/fenced before
  suspension; after resumption the continuation must reacquire a
  generation-checked scheduler ticket and fresh valid state access.
  Callback-wide atomicity **does not** span an `await` that yields.
- `requires bounded, no_alloc, no_block` is an independent verified
  real-time guarantee, not an effect or magic annotation. Unsound admission
  without target proof must fail closed. Do not weaken existing `pure`,
  `nlex`, `trap`, cancellation or supervisor-only PANIC contracts.

## Required compile-negative cases

1. `self.handle_state(|s| -> s.count)`: missing critical callback.
2. `self.handle_state(critical || -> ...)`: missing state parameter.
3. `self.handle_state(critical |a,b| -> ...)`: extra state parameter.
4. `self.state.count` / returning or capturing `s`: state capability escape.
5. `using state.read` but `s.count = ...`: understated effect row.
6. Duplicate/unknown `using` entries and variables named `using` or
   `critical`: reserved/invalid.
7. `await` with live mutable state loan or without `using suspend`:
   no unverifiable readmission.

## Implementation and parity gate

- Java/Graal source branch extends `LambdaExpr` with a `critical` flag,
  parses `critical |s| -> ...` and `critical (s) -> ...`, and enforces
  it at the sealed actor-state verifier, type-checker and conservative
  declared-effect checker. `critical` and `using` are lexer-reserved.
  Existing runtime-backed callback/continuation lowering is **not** implied.
- LLVM native branch provides a typed `CriticalHandleStateContract`
  admission policy and tests effects, critical flags, and suspended-loan
  fencing. Its current small i64 compiler still **rejects** actor
  `state` declarations; host policy conformance is not LLVM guest lowering.
  LLVM reserves `critical` and `using` in its frontend. Exact shared
  guest fixtures and full native compiler execution remain release gates.
- Runtime parity requires generation-fenced scheduling, continuation-aware
  state loan proof, a single-flight parent/actor execution lane, and
  identical emitted source-level behavior across Graal and LLVM. Neither
  backend should claim this is complete on the basis of parser/ABI tests.

This contract intentionally does not change actor `on_start`, `tear_down`,
`on_throw`/`on_raise`, supervisor-only PANIC or response-output
capabilities. HTTP SSR and JSON writers are I/O effects and cannot inherit
state loan authority or use an untrusted raw socket/FD.
