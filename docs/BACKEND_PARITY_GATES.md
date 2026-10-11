# Oreslang GraalVM ↔ LLVM parity gates

**Audit date:** 2026-10-10. **Status: NOT AT FEATURE, SEMANTIC OR BEHAVIORAL PARITY.**
This is a verification ledger, not a release announcement. Both source
repositories must maintain the same copy of this contract.

## What "parity" means

A feature is **feature-parity complete** only when both compilers accept the
same documented source construct and reject the same invalid programs.

A feature is **semantic-parity complete** only when they enforce the same
types, exceptional control effects, ownership/borrowing, actor permissions
and cleanup/cancellation ordering.

A feature is **behavioral-parity complete** only when both backends actually
execute matched fixtures and produce equivalent values, failure categories,
suppressed cleanup order, lifecycle/CONTROL traces and cancellation outcomes.
Backend-agnostic helper classes, reserved tokens and deliberately rejected
syntax are **not** evidence of behavioral parity.

## Current gate ledger

| Surface | Java/GraalVM | LLVM native | Gate |
| --- | --- | --- | --- |
| Typed `int` function/call/return subset | Implemented | Executable i64 subset | Differential execution not yet certified |
| Scalar `rt copy/take/share` | Language/runtime forms | Native i64 admission and borrowing subset | Cross-backend ownership trace gate outstanding |
| `try/catch/finally` | Synchronous and suspension-aware paths, regressions | Rejected; no unwind IR | BLOCKED |
| `throw`, `raise`, `panic` source statements | Keywords reserved; source statements rejected | Keywords reserved; rejected | BLOCKED; no source feature |
| Host panic/raise markers | Distinct runtime signals | Distinct native `FailureKind` values only | Runtime representation partially aligned |
| `defer` unwind | Runtime cleanup and precedence tests | Native policy value-combining only; no source lowering | BLOCKED |
| `trap` | Synchronous `Option` boundary | Rejected | BLOCKED |
| `recover` | Reserved, typed capability boundary unimplemented | Reserved; rejected | BLOCKED |
| `await` / `select` / channels | Suspension-aware Java runtime paths | Rejected; native continuation frames missing | BLOCKED |
| Source actors (shared/isolated/untrusted) | JVM actor execution exists, physical heap guarantees vary | Source actors rejected; carrier primitives alone | BLOCKED |
| Failed child CONTROL notice | Bounded, data-only direct-parent queue and non-cancellable notice Future | Data-only `Failure` policy primitive, no actor delivery | BLOCKED |
| `on_throw/on_raise/on_panic` supervision | Actor method spellings rejected until checked handler dispatch exists | Actors rejected | BLOCKED |
| Error precedence and suppressed diagnostics | `OresExceptionPolicy` | `ExceptionPolicy.hpp` | Policy primitives aligned, NOT source/runtime parity |
| Supervisor diagnostic UTF-8 size | 1,024-byte Unicode-safe cap | 1,024-byte cap and invalid-UTF-8 normalization | Policy primitive gate only |

## Mandatory gates before declaring parity

1. **Source admission:** same supported grammar and typed effect signatures;
   negative tests fail with a nonzero compiler exit and no emitted code.
2. **Control-flow equivalence:** test `throw/raise/panic` through `try`,
   `trap`, nested `defer`, `finally`, nested invokes, `await`, channel
   operations and cancellation. Assert identical classification and all
   suppressed cleanup failures.
3. **Ownership and lifetime:** verify borrows cannot escape scopes or survive
   forbidden suspension; independent actor heaps and deterministic native
   reclamation require physical runtime evidence, not naming conventions.
4. **Actor lifecycle:** startup, handler and teardown faults; exactly-once
   terminal notices, parent/sibling isolation, cancellation and bounded
   priority CONTROL processing without data-mailbox starvation. Exhaustive
   `on_*` handlers must be compiler-verified before source admission.
5. **Differential execution:** run the *same* source fixture with both
   production compilers, compare typed outcome traces and process exit,
   and retain the exact file blob SHA + PR-head SHA for both.
6. **Release gate:** positive tests on both backends, negative reject tests,
   native CTest and current-head GraalVM Maven suites must pass together.
   No benchmark or parser-only test may substitute for missing semantics.

### Current verified scope

Java actor failure notices and suspension-aware exception regressions have
been exercised on a mirrored GraalVM CI runner. LLVM native exception policy
has been compiled and executed with C++17 `-Wall -Wextra -Wpedantic -Werror`.
These are independently useful safety improvements; they **do not** establish
equivalent native actor execution, source effects, async or exception lowering.

Until every blocking row is implemented and differentially tested, maintain
the source PRs as drafts and describe both compilers as **not yet at parity**.
