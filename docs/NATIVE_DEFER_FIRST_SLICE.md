# Native defer first executable slice

Status: experimental, synchronous-only, and narrower than Java/GraalVM.

LLVM now admits function-body `defer call(...);` and `defer owner.field = expr;`
between local declarations and guarded/fallthrough returns. Calls must target
direct same-module functions; deferred field updates require a statically
known, addressable class owner with mutable access. Registration does not call the function. Each early return
and fallthrough evaluates its return value, runs all registered deferred
calls in reverse order, then releases lexical locals and owned regions.

Return expressions are evaluated first and captured as SSA values; subsequent
deferred field mutations cannot replace the pending scalar return. The ownership
checker independently evaluates defers against the live state
of every outgoing return path. A value moved by a return expression cannot
subsequently be borrowed by deferred cleanup. The regular call-site borrow
and exclusive-loan restrictions remain in force.

Java/Graal also supports deferred closures and cleanup/finally control flow,
which remain unsupported in this native subset. The compiler still rejects
source throw/raise/panic, try/catch/finally,
nested defers, branch-local registrations, suspension across defer and
typed cleanup failure precedence. Issue #143 tracks this follow-on work.

The native executable test mutates an owned record on both early and
fallthrough return paths. The final value 111 demonstrates two LIFO cleanup
sequences with real Clang execution.

The ownership CFG verifier also incorporates each deferred call's input
place-read and loan effects **after** the return-expression effects on every
exit edge. A three-exit executable fixture checks two guarded returns plus
fallthrough without granting a moved value a synthetic surviving loan.

## Shared deferred field/return fixture

`tests/fixtures/native-defer-field-return.ores` is byte-identical to
Java/Graal `contracts/v1/semantics/fixtures/native-defer-field-return.ores`.
Both backends must return **222**: the first guarded return snapshots `1`,
its two LIFO field updates produce `11`, the second return snapshots `11`,
and the next LIFO updates produce `111`. Native checks also reject writes
through a shared borrow and writes to a field moved by the pending return.
This is *subset parity*, not general `try/finally`, async, or cleanup-exception
parity. Unsupported forms must fail closed before native IR emission.
