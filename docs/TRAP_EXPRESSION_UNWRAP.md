# Expression `trap` and `unwrap()` — shared source contract

**Status:** Approved language-design target / **not implemented** as expression syntax.
The Java/GraalVM frontend currently implements synchronous **callable** `trap`,
not `trap expression`. The LLVM frontend reserves `trap` and rejects the
unsupported syntax. This file does not claim executable cross-backend parity.

## One rule: catch ordinary `throw`, never panic

`trap expr` is proposed as a unary expression operator whose purpose is
exactly the same as the existing `trap fnc` callable boundary:

- Evaluate `expr` exactly once.
- If it completes normally with value `v: T`, yield `Some(v): Option<T>`.
- If an explicitly admitted ordinary Ores `throw` escapes during that
  evaluation, run lexical cleanup and yield `None: Option<T>`.
- `trap` is an error boundary, **not** a transaction: writes, I/O and other
  side effects before the throw remain visible unless the operation has
  its own rollback protocol.
- `panic`, `raise`, cancellation, forced termination, VM-fatal errors and
  unknown native/host failures **bypass** `trap`; never reclassify them as
  `None`.
- `trap` is not an implicit `Result`-to-`Option` conversion. Values such
  as `Err(e)` and `None` are ordinary successful values of their own type.
- No flattening: `trap expr` always adds one `Option` layer, even if
  `expr: Option<T>`, `expr: Result<T, E>`, or `expr: void`.

Proposed static typing: `Gamma |- e: T` implies
`Gamma |- trap e: Option<T>`. The ordinary-throw effect of `e` is
eliminated at the boundary; other exceptional/control effects are preserved.
For `void`, the language-level result is `Option<void>`, consistent with
the existing compiler-produced callable-trap form.

`trap` binds as a prefix unary operator, including member calls: `trap
account.try_lock().unwrap()` traps the full `unwrap()` call, **not** a
larger surrounding binary expression. Use `trap (a() + b())` to trap
both operands. Parentheses do not change which *kinds* of failure it traps.

## `unwrap()` remains an assertion, not a recoverable failure

`Option<T>.unwrap(): T` returns the `Some` payload or **panics** on
`None`. `Result<T, E>.unwrap(): T` returns the `Ok` payload or
**panics** on `Err`. Expression `trap` must never special-case an
`unwrap()` invocation or silently downgrade that panic.

Assuming the account's `try_lock(): Option<Guard<Balance>>`:

```ores
let mut balance = account.try_lock().unwrap();
    // Guard<Balance>. None -> panic.

let mut balance = trap account.try_lock().unwrap();
    // Option<Guard<Balance>>. Some(guard) on normal success.
    // None from try_lock().unwrap() -> PANIC, not trapped None.
    // An ordinary throw during evaluation -> trapped None.

let mut balance = account.try_lock();
    // Option<Guard<Balance>>. None remains ordinary data.

let safe = account.try_lock().unwrap_safe();
    // Result<Guard<Balance>, OptionUnwrapError>.
```

For a `lock()` API that already returns `Option<Guard>`, the same
types apply. For Oreslang's built-in `Mutex<T>.lock()`, which currently
returns a `MutexGuard<T>` directly, appending `.unwrap()` is a type error;
`try_lock()` is the built-in optional acquisition path.

Examples that must not be confused:

| Expression | Type | On `None` from `try_lock()` |
| --- | --- | --- |
| `account.try_lock()` | `Option<Guard>` | `None` |
| `account.try_lock().unwrap()` | `Guard` | `panic` |
| `trap account.try_lock()` | `Option<Option<Guard>>` | `Some(None)` |
| `trap account.try_lock().unwrap()` | `Option<Guard>` | `panic` |
| `account.try_lock().unwrap_safe()` | `Result<Guard, OptionUnwrapError>` | `Err(...)` |
| `trap account.try_lock().unwrap_safe()` | `Option<Result<Guard, OptionUnwrapError>>` | `Some(Err(...))` |

If the design goal is to represent lock unavailability as `None`, retain
the original `Option` instead of inserting `unwrap()`. Do not introduce
a context-dependent version of `unwrap()` inside `trap`.

## Runtime failure classification and diagnostic integrity

An arbitrary host-language `RuntimeException` is **not** an Ores `throw`.
The current Java bootstrap explicitly treats `OresCastError` (failed
checked cast) as a recoverable guest error for compatibility until typed
`Throw<E>` is admitted. A scheduler interruption, cancellation, panic, `raise`, host-fatal
error, or unknown runtime control must neither become `None` through
`trap` nor be intercepted by ordinary guest `catch`. An explicit
guest `catch` may handle permission denial or a recoverable timeout
without granting the denied operation; `trap` must not silently
downgrade either into `None`. Java's bootstrap admits some ordinary
runtime exceptions for compatibility until source-level typed effects
are implemented and proven on both backends.

In particular, no code path is allowed to turn an `unwrap(None)` panic
into a recoverable error before it reaches `trap`: the control category is
determined by the failure's origin, not by its textual message or by whether
an `Option` happens to be expected.

This rule applies **before** trap classification, including synchronous
`try/catch/finally`, suspended source-task `try/catch/finally`, `defer`
stacks, and mutex-guard release. Run all cleanup, but retain the strongest
in-flight failure and attach lesser failures as suppressed diagnostics,
with a finite diagnostic budget so untrusted code cannot produce an unbounded
list of failing defers. A `finally` cast error may not erase an earlier panic/cancellation,
and a `finally` or `defer` panic outranks an earlier ordinary cast error.
Unknown host/internal failures are not ordinary merely because cleanup
encounters them.

A suspended expression has a **single terminal completion** even when an
await, cancellation, or actor wake races with another terminal event. Once
the result has been handed to its continuation, a failure from that
continuation belongs to the consumer: it must not be reclassified as an
expression failure and dispatched to the same continuation a second time.
Nested asynchronous blocks must likewise run their defer stack and
resource-release finalization once, including exceptional exits.

## Ownership, execution, and fail-closed rollout

`Option<Guard>` is still affine when `Guard` is affine. Converting a
trapped successful evaluation to `Some(guard)` must transfer the guard
exactly once. Failure paths must unwind and release any acquired resources
exactly once. `Option<Guard>` is **not** a usable guard until the caller
checks `Some` and explicitly extracts the owned guard. Merely writing
`let mut balance = trap ...` never grants access to `balance.value`;
unwrapping such an Option must still obey the existing panic and move rules.
The trap boundary cannot outlive its permitted actor/isolate
execution domain or silently capture a detached task's exception.

The first implementation should support only synchronous expressions and
reject suspending/async `trap` expressions until continuation ownership,
cleanup, and post-`await` effects are proved for both backends. No implicit
conversion of panic to throw; no global host exception catch.

## Conformance gates before calling the syntax implemented

1. Parser precedence, single evaluation, and exact `Option<T>` typing.
2. Success `Some(v)`, ordinary throw `None`, no automatic nested flattening.
3. `unwrap(None)` and `unwrap(Err)` panic across `trap`, including
   nested calls, while unknown host and security failures remain untrapped.
4. `raise`, `panic`, cancellation, and termination bypass `trap`.
5. `unwrap_safe()` stays `Result` data, including nested `Option<Result<...>>`.
6. Move-only guards: no double consumption, no leaked resources on unwind,
   correct cleanup ordering.
7. Unsupported async, unsafe host failures, and non-admitted syntax fail closed.
8. Identical source fixtures run against the actual GraalVM and LLVM
   production frontends; no claims of parity from docs or mock lowering.

Also see the Java/GraalVM `docs/TRAP.md` callable-trap contract and
`docs/LANGUAGE.md` Option/Result semantics.
