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
