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

**Compiler/bootstrap integration added in this draft:** `LlvmCodegen.cpp`
now emits the `oreslang_source_actor_spawn_checked` registration, checked
field-read/write calls and explicit LLVM fault/success branches in native
actor handlers. `SourceActorBootstrap.c` implements the opt-in
`OresSourceActorCheckedMessage` ABI, preserves legacy `int64_t` callbacks,
and on a typed nonzero status attempts supervisor-only heap fault retirement
and seals the affected actor's DATA mailbox. It publishes atomic
`successful_messages`, `failed_messages` and `fault_status` observations.
The scheduler's `processed_user_messages` still means *physically executed
callback turns* (including one failed callback), **not successful messages**;
the success/failure counters split those outcomes.

**Critical remaining limitations:** the callback is not yet registered
through `ores_native_vm_spawn_supervised` and does not publish a
generation-fenced `OresNativeActorTerminalFailure` to the root CONTROL
supervisor. A checked `BUSY` result is currently terminal rather than a
scheduler-yield/retry continuation. If supervisor fault retirement itself
returns `BUSY`, the trusted bootstrap still aborts; the checked ABI does not
protect against invalid native pointers, arbitrary C faults, actor stack
overflow, unchecked constructor/scoped-local runtime calls or general
`suspends`/async frame faults. The new native guest regression therefore
proves only successfully executed checked handler calls, and the host C fault
fixture proves only the bootstrap fault path. Neither constitutes complete
actor-local guest failure containment or native broker certification.

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

- Generalize the current **checked actor-field** codegen to checked
  scoped-owner allocation/drop, constructors and all guest-reachable
  ownership operations; enforce compile-time borrowing and no mutable loan
  across `await` before IR emission.
- Extend the now-failure-aware source-actor trampoline to register immutable
  parentage, publish a generation-fenced terminal fault through the existing
  supervised native VM CONTROL mechanism, and distinguish failed DATA turns
  in scheduler terminal accounting (not merely the source-actor counters).
  Implement nonblocking scheduler retry for `BUSY` without losing the
  current suspended guest frame.
- Prove deterministic quiescence of continuation frames, shared readers,
  async completions and actor-owned heaps before physical destruction.
  A failure in the current turn cannot reclaim memory another carrier
  still holds.
- Execute actual compiled `.atto` actors, O0/O2, negative mutations,
  sanitizer coverage, timer/restart/cancellation races and sibling
  survival, binding results to exact compiler/fixture trees and binaries.

No private source transfers are authorized by this document.
