# Trusted same-tenant actor buffer transfer v0

Status: **C++17 host-only substrate**, not Oreslang source lowering, a production mailbox path, tenant isolation, or a stable guest ABI. This file and the CTest are on a draft feature branch.

## Motivation and research

- V8's heap sandbox uses checked/indirected memory addresses; it is different from merely constructing separate V8 isolate objects: https://v8.dev/blog/sandbox
- Wasmtime's pooling and CoW snapshots demonstrate how to avoid repeated expensive initialization, but separate instance memories do not automatically provide actor ownership: https://docs.wasmtime.dev/examples-fast-instantiation.html
- Agnocast (2025) explores shared-memory zero-copy middleware, where producer/consumer lifetime and access rights are the core correctness obligations: https://arxiv.org/abs/2506.16882
- Oreslang's existing [native actor send ownership](NATIVE_ACTOR_SEND_OWNERSHIP.md) and [serde boundary](NETWORK_AND_ACTOR_SERDE_BOUNDARIES.md) require send admission, cancellation rollback and distinct Sendable versus Serializable checks.

## TrustedActorBufferPool

`src/main/cpp/oreslang/runtime/TrustedActorBufferPool.hpp` implements a fixed-byte-budget, fixed-buffer-count native pool owned by one tenant incarnation.

- `allocate(actor, bytes)` creates one independently allocated byte vector charged against a pool-wide budget. Initial allocation can call `new`; it is not a zero-allocation path.
- `write/read(handle, actor, offset)` check pool identity, handle generation, live registration, actor owner, bounds and retirement. The actor identity is **host-supplied and not authenticated by this type**.
- `take(handle, from, to)` changes exclusive ownership, increments a nonwrapping handle generation, and returns the new ticket. It does not allocate or copy payload bytes. All copied aliases of the sender's ticket immediately become stale. The implementation does not enqueue mail or reserve any destination mailbox.
- `borrow_read(handle, owner, reader)` creates a move-only, bounded-by-buffer-lifetime immutable read loan. The loan holds backing storage alive and checks the expected reader on reads. Outstanding loans deny mutation, transfer and drop. It must remain in a trusted synchronous execution scope; cross-await/suspension is **not** supported.
- `drop` releases budget only after all read loans are gone. `retire()` blocks new access, zeros live vectors on a best-effort basis, and revokes read loans. Outstanding loans retain backing storage physically until released, preventing UAF. Scrubbing is not a cryptographic secure-erasure promise.

This code does **not** implement a C++ security boundary. Native clients can fabricate `TrustedBufferActor` integers and are not constrained from reading arbitrary process addresses. A production adapter MUST take authenticated actor-incarnation/tenant identities from the runtime's scheduler ticket, never guest-supplied fields.

## Send transaction ordering (future integration)

1. Resolve the sender and recipient through trusted, generation-fenced actor references. Require same physical trusted tenant context for this fast path.
2. Validate transitive `Sendable<T>` and forbid raw pointers, arbitrary host objects, executable callbacks, CONTROL authority and foreign memory.
3. Reserve byte/slot mailbox capacity and pin the recipient incarnation. Fail with **sender still owning the buffer** if any of these checks fail.
4. Under one publication barrier, call `take()`, enqueue the new capability into the destination mailbox, and publish the acknowledgment only after both succeed. A failure must rollback without exposing two owners; never send the old ticket.
5. Receiver reads under the owner actor's admitted turn; retire, cancellation, STOP and hot upgrade must fence in-flight transfers and immutable loans before physical reclamation.
6. Cross-tenant / untrusted / network messages use typed serialized copies by default. Shared memory requires a separate, audited page-grant broker and bilateral quota/lease accounting.

## Test and measurement

CTest: `native_trusted_actor_buffer_pool` compiles with assertions enabled, -Wall/-Wextra/-Wpedantic/-Werror and C++17.

It checks invalid actors, memory/slot quotas, byte bounds, foreign pool, wrong owner, stale old aliases, an immutable loan concurrently read from a second thread, loan-gated mutation/move/drop, 100,000 owner flips, borrower replay/mismatch, shutdown/revocation, idempotent retirement, and lifetime safety when the pool object is destroyed before a read loan.

The test additionally instruments global C++ `operator new` during a controlled pure-ownership-move loop: the hot `take` path must allocate no heap objects via global `new`. This does **not** establish zero allocations in all runtime paths, zero syscalls, zero GC, or zero payload-copy for a complete actor message.

Release gates: compile and run exact-branch CTest with both GCC and Clang, ASan+UBSan+TSan where supported, then link a true compiled `.ores` actor-send fixture. Add failure injection at mailbox reserve and cancellation publication. Require measured payload-copy counts, allocations, message p50/p99, memory budget and cancellation linearizability before declaring a source-language zero-copy feature.

## Added bounded DATA mailbox host integration

`TrustedActorBufferMailbox.hpp` now has a preallocated ring of buffer tickets, one trusted recipient identity, an explicit close barrier and a strong pool-lifetime reference.

- `send` locks the publication boundary and rejects closed/full queues **before** calling `pool.take`. A rejected send cannot consume the sender's ownership.
- Accepted sends do not allocate or copy payload bytes after the mailbox has been created. A C++ static assertion requires the handle move assignment/publication to be noexcept.
- `receive` is restricted to the expected actor incarnation; a wrong-generation recipient cannot dequeue DATA.
- `close` atomically closes admission then retires unreceived receiver-owned buffers. A racing send is either rejected while the sender retains ownership or accepted and reclaimed by close; there is no split publication.
- This is a host-only immediate-admission DATA queue, **not** async mailbox backpressure/futures, compiler-lowered actor messaging, a hardened actor-tick security boundary, or a cross-tenant queue.

CTest: `native_trusted_actor_buffer_mailbox` covers full-mailbox rollback, borrowed-buffer send rejection, stale handle checks, 10,000 forward/back moves, wrong actor incarnation, queued teardown, and 64 concurrent close/send races. The exact Git blob header and test both compiled and passed locally with GCC and Clang, including ASan+UBSan+leak checking; this is still not full source-org CI acceptance.

**Remaining highest-priority gate:** the authenticated scheduler must mint actor-incarnation authority and bind the mailbox to a real target registration. Add source-compiled `.ores` `rt take` sends, cancellation/STOP, multi-producer fairness, bounded await continuations and endpoint lifetime pinning before exposing this component as a language feature.
