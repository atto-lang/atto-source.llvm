# Immutable actor payload descriptor — LLVM native host v1

Status: **host primitive and CTest, NOT compiled .atto guest parity**.

## Scope and memory model

- Each actor keeps its private arena. This primitive does not grant access to another actor's arena and does not weaken sealed-state rules.
- SharedImmutablePayload::publish(vector<uint8_t>&&) MOVES a caller-owned byte allocation into a read-only shared backing. The allocation is bounded to 16 MiB. No mutable byte pointer is exposed to guest code.
- Descriptor copies and checked slices hold a reference-counted read-only backing. Slices do not copy payload bytes.
- A tiny slice retains the entire slab. SharedPayloadInbox therefore charges the backing allocation size **for each enqueued descriptor**, not the slice length. This is a deliberately conservative quota; caller-specific or tenant-global quotas must be added before production guest traffic.
- Bounded `try_send`, `try_receive_result` and `try_close` provide a host DATA-lane reference model. The DATA slots are now preallocated as a bounded ring; sending does not allocate queue nodes. `try_receive_result` distinguishes `BUSY`, `EMPTY`, `CLOSED` and `RECEIVED` for scheduler integration; the older optional-returning `try_receive` remains a host convenience and must **not** drive readiness/wakeup state transitions. A duplicate descriptor never copies its payload. A queued descriptor is dropped on close; an active receiver's descriptor remains valid.
- `with_read_only_span` is for trusted, synchronous host I/O only. Its callback must return `void`, preventing borrowed span/pointer return values through that API. Trusted callers must still not capture/store a span, and asynchronous I/O must independently retain the descriptor throughout completion/cancellation.
- The host implementation still uses mutexes and `shared_ptr` control blocks. Construction allocates slot metadata; final reference release and `try_close` may reclaim backing memory, while `std::shared_ptr` reference counting uses atomic synchronization. The ring change does **not** establish hard real-time WCET, fully nonparking teardown, full-process zero-allocation, no kernel-to-userspace copy, or zero-GC certification. `try_close` requires external STOP/quiescence.

## Conformance and negative tests

tests/native_shared_immutable_payload_test.cpp is registered with CTest as native_shared_immutable_payload_host. The host test covers:

1. Move publication retains the same data pointer, without a second byte allocation.
2. Read-only reference sharing, checked slices and overflow-sized indices.
3. One MiB backing charged even for a 99-byte slice; bounded byte credits and slot limits.
4. Idempotent close; release of queued handles; active receiver lifetime after close.
5. 8 host reader threads × 5,000 shared-descriptor/slice reads.
6. 100 payload admissions and transfers without byte copying.
7. Zero-byte payload and 16 MiB allocation limit.
8. 512 ring wraparounds, FIFO ordering, quota refunds, and explicit empty/closed receive states.

Local, standalone test run (NOT exact-tree GitHub Actions proof): GCC/Clang, O1/O2, C++20, pthread, ASan+UBSan.
All four local variants passed on 2026-11-12. This establishes native host-model behavior only.

## Integration gates, not yet satisfied

- Bind a mailbox descriptor to an authenticated actor incarnation, source-type Sendable, a supervisor-owned shared domain and an explicit rt share authority.
- Integrate this descriptor with the existing native actor DATA mailbox and its CONTROL priority semantics; do not use a parallel guest mailbox or bypass admitted sends.
- Compiler typecheck/lowering for Bytes, rt take and rt share, including rejection of mutable aliases, use-after-move, cross-isolate pointers, and borrowed values across await.
- Quota accounting per tenant/domain; cancellation and STOP races; deterministic destructor cleanup; ABA/generation rejection; post-close publication fencing.
- Non-blocking TCP reactor integration with read-lifetime pins and scatter/gather writes. Completion must retain backing until I/O acknowledgment or cancellation.
- Real .atto guest compiler + native AOT execution at O0/O2, ASan/UBSan, exact-head full tree CI, and Java/.NET/WASM/Forge conformance receipts.

Do not promote this host-only CTest into a guest parity claim. Existing arena and slab systems remain authoritative until integrated.
