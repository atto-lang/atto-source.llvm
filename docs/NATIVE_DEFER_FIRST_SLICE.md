# Native defer first executable slice

Status: experimental, synchronous-only, and narrower than Java/GraalVM.

LLVM now admits a function-body `defer call(...);` between local
declarations and guarded/fallthrough returns. Only direct same-module calls
are permitted. Registration does not call the function. Each early return
and fallthrough evaluates its return value, runs all registered deferred
calls in reverse order, then releases lexical locals and owned regions.

The ownership checker independently evaluates defers against the live state
of every outgoing return path. A value moved by a return expression cannot
subsequently be borrowed by deferred cleanup. The regular call-site borrow
and exclusive-loan restrictions remain in force.

The compiler still rejects source throw/raise/panic, try/catch/finally,
nested defers, branch-local registrations, suspension across defer and
typed cleanup failure precedence. Issue #143 tracks this follow-on work.

The native executable test mutates an owned record on both early and
fallthrough return paths. The final value 111 demonstrates two LIFO cleanup
sequences with real Clang execution.

The ownership CFG verifier also incorporates each deferred call's input
place-read and loan effects **after** the return-expression effects on every
exit edge. A three-exit executable fixture checks two guarded returns plus
fallthrough without granting a moved value a synthetic surviving loan.

## Deferred field writes and return snapshots

The synchronous subset now accepts `defer namedOwner.field = expression;`
with the same field-mutation/loan checks as ordinary writes. Every return
expression is evaluated into an SSA snapshot before LIFO deferred writes; a
deferred mutation cannot replace the pending return. The exact shared guest
fixture `native-defer-field-return.ores` returns 222 on LLVM and Java.
Nested/deferred closures, typed `finally`, async cleanup and exceptional
unwinding remain fail-closed.