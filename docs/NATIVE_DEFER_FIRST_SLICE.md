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
