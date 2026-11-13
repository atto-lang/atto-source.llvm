# Native actor i64 loan fault-status ABI — experimental host primitive

This file describes the **trusted LLVM native C runtime**, not a completed
AttoLang guest-language effect, memory-safety proof, or cross-actor shared
memory API. The implementation is in `ActorOwnedI64Heap.{c,h}`. It is a
bounded prerequisite for actor-local guest fault delivery, but does not
by itself make a `longjmp`, a C `abort()`, or an uncaught native signal
safe inside a shared VM process.

## Admission and status contract

`ores_actor_i64_heap_try_borrow_mut` returns `1` and a fresh loan only
when the turn has exclusive access to an owned i64 slot. It returns `0`
immediately on a held reader, another writer, full 64-loan quota, or a
contended heap lock, and zeros the proposed output token. This legacy
try-API can still abort on invalid input and **must not** be used as an
untrusted guest-fault branch.

Checked `ores_actor_i64_heap_try_loan_read_mut` and
`ores_actor_i64_heap_try_loan_write` return a typed
`OresActorLoanStatus` without abort on the following ordinary admission
failures:

| Status | Meaning | Required compiler/scheduler behavior |
| --- | --- | --- |
| `ORES_ACTOR_LOAN_OK` | Authenticated active turn + live exclusive loan | Continue |
| `ORES_ACTOR_LOAN_BUSY` | Heap lock unavailable on single attempt | Yield and re-attempt through scheduler policy; never busy-spin |
| `ORES_ACTOR_LOAN_BAD_TURN` | Missing/stale/wrong domain or epoch | Fault the owning actor; do not complete DATA as successful |
| `ORES_ACTOR_LOAN_BAD_HANDLE` | Invalid/released/foreign/forged loan token | Fault the owning actor; do not complete DATA as successful |
| `ORES_ACTOR_LOAN_RETIRED` | Heap already fenced against work | Drop or cancel the continuation; never reacquire actor state |

A failed read clears the output scalar. A failed write changes no heap
slot or per-loan access-trace entry. These functions never expose host
pointers to guest code. The underlying `OresActorI64Heap*` must remain
alive by supervisor lifetime pinning; passing dangling heap pointers is
outside this API's safety envelope. Source ownership checking and
compiler-issued, non-forgeable logical handles remain required.

## Supervisor-only fault turnover

`ores_actor_i64_heap_fault_turn(heap, authenticated_turn)` is a
one-lock-attempt operation called only after the owning actor's guest
handler has been interrupted at a checked boundary. On success it:

1. Verifies the current actor heap domain/turn epoch.
2. Revokes and zeroes *all* of the failed turn's exclusive loan records,
   checking slot generation and ledger consistency.
3. Marks the turn inactive and the heap retiring, rejecting later turns.
4. Leaves cross-turn shared read loans intact until each holder is fenced
   and explicitly releases it.

`ores_actor_i64_heap_try_destroy` must return `0` until the heap is
retired, no turn executes, and no shared loan remains; the supervisor must
fence all timers, mailbox work, FFI callbacks and continuations before
calling it. A fault in actor A does not authorize canceling or reclaiming
actor B's heap.

**Critical limitation:** existing source actor `on_message` callbacks
are `int64_t` returning and `SourceActorBootstrap.c` currently assumes
the callback returns successfully, then calls `heap_leave` and accounts
the DATA message as processed. Compiler lowering does not yet emit the
checked status branches or supervisor-fault delivery. The runtime's
legacy fatal APIs still call `abort()` for trusted-host invariant
violations. Therefore these helpers do **NOT** certify actor-local
recovery from arbitrary guest faults, stack overflow or use-after-free.

## Tests and evidence

`tests/actor_owned_i64_exclusive_loan.c` covers two independent actor
heaps, reader/writer exclusion, 64-reader quota saturation/recovery,
checked forged/cross-actor/released/stale loan diagnostics, failed write
non-mutation, failed read zeroing, exclusive-loan retirement, shared-read
quiescence, sibling progress, and 16 fork-isolated hard-invariant abort
negatives. The source-local, read-only
`scripts/test-actor-i64-exclusive-loan-host.sh` checks exact source
Git blob IDs and compiles/runs both GCC and Clang (with UBSan) without
exporting private source. This is **host-only native C** evidence when
a runner actually executes; source-org zero-step GitHub Actions are
`NOT_EXECUTED`.

## Remaining P0 integration before guest certification

- Emit checked loan calls and explicit branch-on-status in the real
  `oreslang-llvmc` Atto compiler. Borrow, mutable state, and `await`
  suspension rules must be enforced at compile time.
- Have the source-actor trampoline distinguish successful DATA completion
  from an actor fault. Route a generation-fenced typed failure through
  the existing supervised native VM CONTROL mechanism; seal the failed
  actor's mailbox without clearing sibling lanes or treating the failed
  DATA message as successfully processed.
- Prove deterministic quiescence of continuation frames, shared readers,
  async completions and actor-owned heaps before physical destruction.
  A failure in the current turn cannot reclaim memory another carrier
  still holds.
- Execute actual compiled `.atto` actors, O0/O2, negative mutations,
  sanitizer coverage, timer/restart/cancellation races and sibling
  survival, binding results to exact compiler/fixture trees and binaries.

No private source transfers are authorized by this document.
