# Native pinned-continuation lifecycle: pre-source-lowering safety contract

Status: **runtime substrate only**. This is neither a compiler-generated Oreslang
continuation frame nor valid evidence of source-level `await`, `select`,
actor spawn/supervision, cancellation-safe borrowed references, or feature parity
with Java/GraalVM. LLVM must keep rejecting unsupported syntax.

## State/ownership gates

- Every continuation has an `AsyncIoTicket {actor_id, generation, operation_id}`.
  Zero IDs, stale actor generations and mismatched operations fail closed.
- Loan acquisition is bounded (1,024 shared or one exclusive) and permitted only
  while the operation is pending. The gate stores no owner pointer and does not
  make an unpinned allocation safe. A compiler proof must supply the pinned
  owner and disallow moving/dropping that owner while a loan is active.
- Completion, cancellation, timeout, failure and close race for **one** terminal
  decision. Even a cancellation winner cannot release the I/O registration or
  physical frame before the broker acknowledges actual quiescence.
- Broker ACK must mean callbacks have stopped and the broker will not touch its
  buffers or emit another completion. ACK cannot be fabricated by user code;
  the native I/O broker integration must enforce this guarantee.
- The actor scheduler claims one resume or cancellation-cleanup turn **after**
  ACK and must separately acknowledge **finishing** that turn.
- Physical retirement requires broker ACK, a finished scheduler turn and zero
  active loans. A claimed-but-unfinished continuation must never be reclaimed.
  Late callbacks may not change terminal state or resurrect retired handles.

`PinnedContinuationGate` is an allocation-free, lock-free metadata gate
stored *inside* a separately owned pinned frame. It is not a lifetime owner:
the caller must keep the frame allocated for every in-flight callback and
must never let a broker access raw actor-owned objects.

## Verification

`native_pinned_continuation_loan_cancel_quiescence` executes 1,200
three-way completion/cancellation/timeout races, stale ticket rejection,
shared/exclusive conflict checks, broker-ACK ordering, exactly-once
resume/cleanup, no early frame retirement, no double release, and bounded
loan capacity. It uses real `std::thread` races and must pass with
`-Wall -Wextra -Wpedantic -Werror`, plus CTest and sanitizer CI.

## Remaining issues #45 and #14 integration gates

1. Define typed HIR places, field-sensitive CFG loans and pinned actor-local
   continuation frame layout, with compiler-enforced owner provenance.
2. Lower `await`, `select`, timeout and cancellation into a state machine;
   arrange broker registration retirement, losing select-arm unlink and
   scheduler-only guest continuation invocation.
3. Connect generated frame IDs to the authoritative ActorGroup scheduler and
   native actor heap. Do not split state across a new actor registry.
4. End-to-end identical `.ores` source fixtures across Graal and LLVM with
   native ASan/UBSan and cancellation/race/replay testing. The C++ tests in
   this PR do **not** satisfy these end-to-end requirements.
