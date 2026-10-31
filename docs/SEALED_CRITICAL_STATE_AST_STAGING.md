# Native sealed-state typed AST staging (not source parity)

LLVM PR #1219 now introduces **typed AST placeholders** `SealedStateSchemaDecl`, `SealedStateFieldDecl` and `CriticalStateAccessDecl` in `frontend/Parser.hpp`. The sealed schema is intentionally distinct from legacy scalar actor fields.

`frontend/Parser.cpp::sealed_state_schema()` lexes `state { field: int = signed_integer; ... }` into a bounded source-only schema and rejects duplicate field names, empty schemas, over-capacity, unsupported types and 64-bit overflow. The parser still ends with the existing diagnostic:

```
native actor state { ... } requires sealed handle_state lowering
```

This is deliberate: `ActorDecl.sealed_state` and `ActorDecl.critical_state_uses` contain **no runtime identifiers or tickets**. Only the scheduler/runtime can mint execution/generation authority. A typed schema without compiler lowering and runtime admission **must not** produce a supposedly safe actor. In particular `critical[read]` and `critical[write]` callbacks are *not parsed* or executable yet.

The exact source fixtures added to `tests/fixtures` enforce that even a valid signed-MIN schema is not lowered and that malformed schemas reject deterministically. A native source compiler CTest success here means **fail-closed boundary**, not guest parity.

## Required next changes before positive native guest execution

1. Add fully typed sealed-state actor `receive` signature, parameterized `ActorMail<T>`, and inline `handle_state(critical[read/write] |state| -> ...)` statements in the parser and ownership-checking AST. Preserve naming conventions and source-mode identity.
2. Teach the ownership CFG to emit `RegionLoanRequirement` with symbolic state binding, current logical execution and mandatory generation fence. Never inject a compile-time numeric execution handle or fabricate an actor heap identity.
3. Generate matched acquire/validate/read or write/release operations with lexical cleanup on all errors, STOP and cancellation. The native task-bound region runtime from PR #1192 is the only admission authority; #1211 supplies thread matrix regressions.
4. Prove exact guest `.ores` -> LLVM IR -> clang/native executable, plus negative direct-access/mutation/escape/await/reentrancy cases, then native ASan/UBSan and 3/7/15 contention and actor arena retirement.
5. Repeat the same exact source contract across Java (candidate #2335), WASM, .NET Native AOT and Forge, reporting only *executed* per-backend evidence.

At authoring, source-org Actions have pre-runner failures with empty steps; the physical CI test is still unverified. Never export private source to a cross-org CI carrier while destination allowlist/branch-protection gates remain unresolved.
