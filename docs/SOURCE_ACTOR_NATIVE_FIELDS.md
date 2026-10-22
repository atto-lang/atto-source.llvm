# Source actor scalar fields on the current native ABI

The preserved Counter fixture has immutable base/delta and mutable total fields.
It exposes only `pub fnc on_message(int n): int`, the compiler's single admitted
ingress. The older public read/add/plus methods are not part of this native slice.
The host harness calls compiler-emitted init/on_message symbols during admitted
turns, checks totals 45 and 89 across separate turns, then retires the heap.

Use generation-fenced `OresActorSlot` and `OresActorLoan` values for host storage.
The field ABI validates domain, epoch, mutability and outstanding loans before
access. This fixture does not claim general actor method, await, or exception
lowering. `refreshed_actor_persistent_fields` compiles, links and executes it.


## Experimental synchronous owned-i64 handler temporary

Under `--experimental-owned-int`, the LLVM frontend admits up to eight
`const temporary: owned int = <pure integer expression>;` declarations
*before actor field writes* in a synchronous `on_message(int)` handler.
These are true actor-private native slots, not separate C heap regions:
`oreslang_source_actor_spawn` reserves native slot capacity for the persistent
fields plus the maximum simultaneously live temporary owners.

The compiler generates a three-`uint64_t` token on the native handler stack
for each local, calls `ores_actor_i64_heap_scoped_new` during initialization,
and emits a reverse-order `ores_actor_i64_heap_scoped_drop` epilogue before
returning to the mailbox carrier. Drop scrubs native payload and advances slot
generation. The same heap slots may then be reused on the next message
without a guest tracing collector or managed wrapper allocation. Mutable
borrows, ownership transfer, calls from the initializer/return expression,
unstructured early returns and suspension are **not** admitted in this slice.

An immutable scalar copy is retained in a compiler-owned native stack slot
for pure arithmetic while its lifetime token guards the authoritative native
actor heap allocation. This is not zero-copy or first-class source object
lowering: it exercises allocation and deterministic dropping from actual
Oreslang actor source, with deliberately narrow semantics.

The source actor e2e fixture `source-actor-scoped-owned-i64.ores` sends eight
messages while reserving **one field and only two temporary native slots**.
If generated drops are omitted, the next message will exhaust the actor's
fixed heap capacity and abort. The harness also rejects unflagged admission
and attempts to return a borrowed temporary.

**Not yet covered:** `rt take` ownership transfers, true native object
layouts, asynchronous actor handlers, `await` spills, throw/raise unwind,
actor cancellation, FFI loans, duplicate owned handles or compiler-generated
continuation drops. All of those remain required before broad zero-GC
source-language claims or merging without exact-head native tests.


## Checked actor temporary allocation / partial-construction failure

The experimental synchronous native actor owned-local lowering uses
`ores_actor_i64_heap_scoped_try_new` to return an internal success or
capacity-exhaustion status. The native runtime validates the active heap
turn, requires an unpublished zero-initialized destination token, publishes
the token only after completing the slot allocation under its lock, and
returns `0` with the token unchanged when no recyclable native slot remains.
Misused actor authority, nonzero destination tokens, corrupted loan
accounting and stale generational handles are **not** recoverable quota
errors: those fail closed.

LLVM emits one guarded `allocation.status` branch per source owned local.
A failed allocation drops all previously acquired local tokens in reverse
order before a hard `llvm.trap`. The successful handler exit retains its
original reverse-order drops. Neither route publishes a partially
initialized source value or reenters a callback with dropped owners.
The independent native status test verifies quota denial, unchanged output
token, zeroed drop tokens and bounded generation-checked reuse across
10,000 turns. The compiled-source actor stress test checks both the
success-path drops and failure-path drop *instructions*; it also removes
drop instructions in a deliberately broken IR mutant and expects fail-stop.

**The source actor method still returns only int.** Its declared `raises`
or `Result` effect cannot be inferred from this internal C allocation
status; source-level error recovery, supervisor escalation and recoverable
cancellation cleanup require effect and continuation lowering that is not
yet implemented. The failure edge therefore cleans up and **traps** rather
than inventing a source-visible exception. This is *failure-atomic fail-stop
cleanup*, not a successful recoverable allocation error protocol.
