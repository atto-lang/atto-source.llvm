# Oreslang finally control-flow contract v1

Status: normative language semantics; Java/GraalVM source implementation and LLVM source-admission coverage have separate conformance gates.

## Lexical rule

A `finally` body runs cleanup on every exit from its associated `try`/`catch`. It cannot transfer control by an enclosing-function `return`, value-arm `return`, `return do match`, or `return do switch`. This is a **compile-time error**, even when the return is nested in `if`, a `match`/`switch` arm, a local block, an inner `try`/`catch`, or an unreachable branch. The rule applies to the entire lexical `finally` region, rather than just its top-level statements.

A lambda expression inside `finally` creates a **new callable/return boundary**. `(|| -> { return "foo"; })()` is valid because its return exits only the lambda; the containing finally continues. The lambda's *own* finally still cannot return.

A `break` or `continue` whose target loop is outside the current `finally` is also forbidden: otherwise it could silently discard a return pending from `try`/`catch`. Local loop control for loops declared inside `finally` remains valid. Nested finally regions apply these restrictions independently.

## Unwind, ownership, and exceptions

A function return or value-arm result selected within `try`/`catch` is preserved while the associated `finally` runs. When finally finishes normally, the pending exit continues to its original **lexical destination**, not to the surrounding catch or a different value arm. A runtime control signal is not a guest exception and must never be caught as one.

Each scoped `defer`, mutex/borrow release, actor resource finalizer, and `finally` body must run at most once for a given unwind (except retries that explicitly begin a new execution). A real exception or panic raised by finally is still a failure path and may supersede a pending return; banning return does not suppress exceptions or promise that failing cleanup is ignored. Ownership and cancellation guarantees during actual async continuation still require separate conformance testing.

## Frontend / backend obligations

- Java/GraalVM parser: reject every source return within lexical finally; reset the restriction only when entering a new lambda callable.
- Java/GraalVM static checking: independently reject illegal AST control exits; forbid break/continue that target outer loops, but permit finally-local loops. A finally must not count as a source of a function return or value-dispatch result.
- Interpreter and resumable source tasks: preserve the pending return or arm-local result when finally completes normally and run cleanup before delivering it; never route an internal return signal into catch as an ordinary exception.
- LLVM: until general source try/catch/finally grammar, unwind, and ownership-safe lowering are implemented, reject this syntax before emitting any IR. Native scalar match/switch support alone is not evidence of finally parity. When the grammar is implemented, all the rules above become mandatory admission and generated-native execution gates.

## Required regression coverage

1. Reject direct returns and return-do forms in finally, including nested match/switch, nested try/catch, and unreachable branches.
2. Accept returns in try/catch and nested lambdas in finally; verify that the original pending result survives.
3. Reject break/continue to loops outside finally; accept a break within a loop declared inside finally.
4. Verify defer/finally ordering and exactly-once cleanup on successful return, error, cancellation, and actor suspension where supported.
5. Exercise ordinary function returns and value-producing match/switch arm returns separately, including negative missing-result proof.
6. Compare identical source programs, diagnostics, and ownership traces across Java/GraalVM and LLVM once LLVM supports this grammar.

These are release gates; funded exact-file mirrors and unit tests do not by themselves certify entire source-tree identity, all native-image modes, or production actor-continuation parity.
