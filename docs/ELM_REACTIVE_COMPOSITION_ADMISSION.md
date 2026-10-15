# Elm composition and reactive interop: LLVM admission

**Draft gate, not native source implementation.** The canonical Oreslang names are snake_case: `flat_map`, `and_then`, `map_err`, `filter_map`, `on_success`.
See common-source PR #36 for the versioned contract and Java PR #222 for the existing draft Option/Result implementation.

The native source compiler has narrower `Result`, `Option`, callback, actor and suspension support than Java. It must reject unimplemented combinations before emitting IR, never erase `Err`, synthesize a `None`, or treat raw Channel as multicast Observable.

`tests/elm_composition_admission.py` checks one known-good native integer program and four **currently unsupported** real Oreslang sources. A rejection must return a diagnostic and zero IR. When native composition is implemented, replace each rejection with a positive typed source->IR->binary execution test plus negative shape, ownership, callback-effect and cancellation tests.

Native lowering later requires tagged payload ownership, exactly-once active-variant destruction, short-circuit callback counts, no borrow/actor escape, safe continuation lifetimes, bounded channel demand and distinct cancellation/complete/failure outcomes. Host-only C++ Option/Result emulations are not guest parity. No general `Monad<F<_>>` typeclass in this phase.

Additional negative admission fixtures cover `Stream.filter_map` and illegal implicit `Channel<T>` to `Observable<T>` conversion. Both are unsupported in native guest code; future admission needs explicit backpressure/subscription and ownership proofs.

## Postfix question mark and typed exceptional control (2026-10-15 hardening)

The source admission corpus now covers eleven cases including postfix
`Result?`, postfix `Option?`, proposed `throws E` conversion inside
`try/catch`, `raise` bypassing local catch, and `panic` escalation.
They remain **negative acceptance gates**: a diagnostic/no output is not
executable evidence of return, throw, recover or supervisor correctness.

See common-source PR #36 (postfix `?` routing contract) together with
common-source #35 (callable `throws`/`raises`/`panics` proposal).
`Result<T,E>?` in a `Result<U,E>` callable returns `Err`, even when
`throws E` is also declared. Only a compatible non-Result callable
explicitly declaring `throws E` may convert an `Err` to an ordinary
`throw`. `Option<T>?` can return `None` only from an
`Option<U>` callable; it cannot fabricate an exception. Ordinary
`try/catch` handles real `throw` only, not `Err` returns.
`raise`, `panic`, cancellation, and fatal controls are not caught by
`?` or converted to stream completion.

Release requires native guest-source lowering, typed effect and ownership
verification, exactly-once LIFO cleanup across early return and exceptional
unwind, cancellation/continuation and supervisor integration, plus verified
matching traces against Java/Graal. Do not promote the current negative tests
to positive execution tests without those proofs.
