# Physical actor termination and native-loan parity

This draft establishes a **narrow native substrate guarantee** shared with
[Java/GraalVM's actor shutdown contract](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/567).
It does **not** implement full Java actor lifecycle parity or source-level
`defer`/`finally` during LLVM cancellation.

## Physical-done requirement

A native actor cannot publish successful physical completion until:

- Its scheduler has fenced new DATA and CONTROL guest admission.
- The last native actor turn has exited and its execution context is no longer entered.
- All borrowed native heap slots and fields have been released.
- The owning native heap has retired and physically been destroyed.
- Host callbacks/loan holders are quiescent (they may not access the heap after destruction).

The bounded native heap `ores_actor_i64_heap_try_destroy()` returns zero if
its turn or any loan is live. Once it returns one, it frees the backing heap;
the pointer is invalid and **must never be dereferenced again**. The caller
owns an external callback/scheduler quiescence barrier. Unlike the Java
`ActorRef.get_done()`, the C API is not itself a guest awaitable Future.

The executable CTest `native_actor_physical_done_two_loan_race` creates two
outstanding loans on separate slots, requests retirement while a turn is live,
verifies physical destruction rejects that state, exits the turn, releases the
loans concurrently on distinct completion threads, waits for both threads to
join, and only then destroys the heap. It repeats this 64 times. That tests
native ownership, **not** Java-to-LLVM compiled source semantic equivalence.

## Deliberate unsupported language boundary

The experimental LLVM compiler supports a narrow source actor-field and
spawn/await state-machine subset; it does **not** yet compile Java's
lexical cancellation cleanup, structured exception `finally`, FFM arena
callbacks, `get_shutdown_result()`, or generalized actor/supervisor
cancellation. Unsupported programs must fail nonzero with **no LLVM IR**
instead of lowering them to silently skipped cleanup.

Specifically, the Java AOT/hybrid fixture `native-await-finally.ores`
is a positive execution fixture **only** for the Java backend. LLVM's
`reject_unsupported_finally_source` CTest is a negative capability gate
until source-level finally/cancellation semantics are implemented. This
document does not claim parity on `self.forward()`/`self.backward()`
typed actor pipelines either; those require actual compiled guest execution
and typed bidirectional message exchange.
