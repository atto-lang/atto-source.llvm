# Native proxy request slab — LLVM host substrate

Status: **physical preallocated primitive-column implementation**, not guest-source lowering or a running reverse/forward HTTP proxy.

`ProxyRequestSlab.hpp` requires an externally owned `std::span<std::uint32_t>` with an exact, checked capacity. It packs eight request fields in **SoA** or 16/32/64-lane **AoSoA**, plus eight fixed pin-receipt lanes per logical request. No heap allocation occurs during `admit`, `pin`, `settle`, `retire` or metadata access. Reclamation uses an O(1) free list. Slot incarnation and receipt serial advance without wrap, preventing delayed completion from affecting reused physical storage. A RETIRING slot is not reusable until all pin receipts are retired. Closing fails while live requests remain and scrubs the backing span after they drain.

The paired Java/GraalVM implementation is [ActorProxyRequestSlab](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1941). Java PRIVATE actors use charged direct actor-arena memory, Java SHARED actors use charged primitive JVM arrays. The LLVM host-only reference does **not yet** call `ActorShardBudget` or the native scheduler's exact-generation actor-turn guard. The caller must provide a charged actor-owned arena, keep it alive longer than the slab, and serialize all mutations to a single logical actor turn. Unsafe foreign carrier calls or reclamation before external I/O drains remain release blockers; this type alone is **not** an actor security/capability boundary. No pointers are exported to the guest language.

## Reproducible native smoke

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
  -Isrc/main/cpp tests/native_proxy_request_slab_test.cpp -o /tmp/native-proxy-slab
/tmp/native-proxy-slab
```

For sanitizer coverage, use `-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined`. Tests cover SoA and tile-16/32/64 AoSoA, full-capacity backpressure, two independent pin claims, retirement while pinned, out-of-order and replayed receipts, old ticket rejection after reuse, complete scrub and seeded 12,000-step churn over eight capacities. This standalone test ran locally under GCC strict C++20 and ASan/UBSan and printed `NATIVE_PROXY_REQUEST_SLAB_OK`. It does **not** establish LLVM guest compiler parity, cross-actor safety, optimized end-to-end latency or HTTP wire correctness.

The `native_proxy_request_slab_host_only` CTest target plus exact-head native CI will reproduce this proof when runners are available. Next steps: actor scheduler fencing, quota-owned arena allocation, guest Oreslang native API lowering, transport completion/drain integration, JDK25 Maven runtime JUnit and exact-head LLVM compiler tests; then measure p99/p99.9 and allocations before choosing default lane sizes.

## Actor-scheduler enforcement added after initial PR

`ActorOwnedProxyRequestSlab.hpp` now wraps the slab, denying every method unless the **exact native scheduler and actor generation** are executing a turn. The wrapper atomically charges `ActorShardBudget` plus VM quota before allocating a primitive backing vector. All normal request operations remain allocation-free; explicit `close` requires quiescence, and destructor aborts rather than freeing backing with live requests. On successful teardown, backing is scrubbed and released **before** budget refund. Foreign actor carriers, outside-host calls and stale actor generations are rejected. The budget ledger and native scheduler must outlive the wrapper, and actor cancellation must first join all external I/O and pending callbacks; a forced process abort is not graceful recovery.

The new `native_actor_proxy_slab_generation_and_quota` CTest runs against the repository's real `NativeActorScheduler.c`, including owner-turn checks, a tiny-budget reject, a pinned cancellation, a different actor attempting to settle a receipt, and post-retirement quota refunds for SoA and AoSoA 16/32/64. Exact-head GitHub Actions also compiles this test with the C scheduler compiled as C.

Local verification **does not establish actual CTest/runner success**: the new header and test passed GCC strict C++20 against an API-compatible, deterministic fake native scheduler; an ASan/UBSan run of that fake-scheduler test also passed. The original allocation-free underlying slab passed its standalone real-header smoke. The real native scheduler plus budget integration must be executed on an authorized repository runner or local complete checkout. The `std::vector` backing is charged native host memory, **not yet an actor-heap allocation from `ActorArena`**; true arena placement and source-level Oreslang compiler lowering remain P0.

**Unresolved callback provenance:** the 64-bit receipt contains lane + serial, not a global slab identity. The transport/mailbox completion dispatcher must bind the receipt to the owning slab/actor generation and reject incorrectly routed receipts. Never accept external/user input as receipt authority or pass raw cross-actor pointers.


## Physical actor arena pages and completion provenance (latest stack)

The native `ActorOwnedProxyRequestSlab` now stores **all eight SoA/AoSoA request columns and the fixed receipt ledger in actual persistent `OresActorArena` slots**. The independent `std::vector` mirror has been removed from this adapter. The backing is paged at the existing 1 MiB per-persistent-slot limit, with total padded slot capacity charged atomically to the exact actor and VM budgets before opening the arena. Construction assigns an owner identity derived from the exact scheduler actor slot/generation and allocates persistent handles while that actor turn is active. Every public adapter operation re-enters an arena turn and the arena verifies domain, epoch and the current physical carrier; this permits actor migration across turns and denies unauthenticated raw access. `close()` requires no live requests and scrubs all physical words; teardown retires/zeroes/frees the arena **before** refunding actor and VM quota.

**Critical performance limitation:** the existing checked `ActorArena` read/write calls acquire a small internal spin lock and copy each scalar; the adapter currently enters/leaves an arena turn per request operation. This is a correctness-first storage integration, not proof of zero-overhead/no-lock hot-path performance. Follow with a compiler/runtime internal **turn-scoped batch/column kernel** once all lifecycle tests pass; never export raw pointers or allow writable cross-actor aliasing.

The completion adapter now returns a by-value `Receipt` containing exact actor scheduler domain, actor slot, actor generation, slab identity, request ticket, and raw claim serial. A received completion must be dispatched to its owning actor, and the adapter rejects wrong slab/actor/request provenance, stale request incarnations, duplicate completion and late arrivals. The host-only raw `ProxyRequestSlab` method remains available only within the trusted actor adapter; **do not lower raw claim settlement into guest Oreslang**. This envelope is correlation metadata, **not a cryptographic capability** or permission to consume untrusted network-provided receipt values. The native scheduler regression deliberately creates the same raw receipt in two separate slabs and verifies cross-slab delivery is rejected.

The updated tests and CMake/CI targets link the repository's real `ActorArena.c` and `NativeActorScheduler.c`. **No passing exact-head CI result or full native scheduler test is claimed** until executable evidence exists. A successful local stub-backed test of the *previous version* is not a passing test of these new changes. Real guest compiler lowering, I/O completion dispatcher, network transport and p99 performance remain unimplemented.

## Terminal cancellation: quiescent fallback cleanup (latest hardening)

An actor can settle/retire all requests and still miss an explicit `close()` if terminal cancellation skips application cleanup. The owner adapter now provides **one narrowly fenced fallback** when its trusted host destructor runs after the exact scheduler-done retirement fence: if `ActorShardBudget.retired()` is true and the slab has **zero live requests**, the destructor enters a fresh checked `ActorArena` turn, scrubs the authoritative physical slot words, then retires/frees the arena before refunding both ledgers. The fallback is forbidden while an actor remains active or any request is live; those remain fail-stop instead of risking use-after-free. The host must not use this to bypass real external I/O quiescence: scheduler retirement must occur only after all callback sources, pins, wakers and external I/O have joined.

The full native scheduler regression deliberately omits `other_slab.close()` after all its pins and requests drain. It destroys that slab **only after** `retire_actor_shard_budget_after_done` succeeds and asserts that both actor and VM quota return to zero. A passing complete native CTest/ASan run is still required; previous isolated checks did not execute this new fallback.

**Private CI policy remains restrictive:** source Java [issue #1501](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/1501) still has no approved exact destination, and security authority [PR #1724](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1724) is held; a successful alternative-org *source-free* runner probe does not authorize private-tree copying. Do not mirror these source files until the independent review and immutable carrier contract explicitly cover the destination and this PR.

## Exclusive physical page-claim receipts (PR #891 follow-up)

The native arena's checked `ores_actor_arena_claim_pages` now atomically validates and claims a complete list of physical slot handles while holding its existing arena lock. Every page must name a distinct live allocation in the **same arena**, match its current generation and exact allocated page length, and be free of outstanding arena loans and previous page claims. Invalid or conflicting requests return zero **before changing ownership or writing any bytes**. Successful claims receive an arena-local monotonically increasing nonzero receipt which never wraps back to a previously used value.

`ProxyRequestSlab` takes the complete multi-page claim **before** writing its column or receipt storage, copies the handle list into private bounded storage, and holds the receipt for its entire open lifetime. A second slab cannot zero or consume any overlapping subset of those pages, including a one-page view over a two-page owner. Disjoint pages can be claimed concurrently in the same valid actor turn. C arena `drop` and persistent `borrow` reject claimed slots; `try_destroy` refuses outstanding page claims.

After all slab request receipts have been settled and `live() == 0`, `close()` scrubs **every byte of the charged physical pages**, then presents the original page set and claim receipt to `ores_actor_arena_unclaim_pages`. That release verifies the full group size and every live generation and claim before clearing **any** claim. Partial release, duplicate/replayed release and guessed or stale receipts fail without side effects. The actor-owned wrapper's terminal cleanup remains subject to scheduler-done and external I/O quiescence fences.

**Authority boundary:** this is exclusivity between **trusted slab adapters**. The low-level `ores_actor_arena_read/write` functions remain trusted host entry points and are not yet fully capability-gated by the page claim; a privileged host holding an arena handle and authenticated actor turn can still access bytes. No private arena pointer or page receipt is a guest-visible Oreslang capability. Do not market this as isolation from malicious or buggy trusted host code.

The `native_proxy_slab_arena_pages_and_zeroization` CTest exercises true two-page storage, conflicting whole and subset reservations, untouched sentinels after rejected conflicts, claim replay/partial-release rejection, post-close reuse, disjoint slabs, and final arena destruction. The exact-head workflow includes its strict C++20 and ASan/UBSan builds. **No checked-in test execution is yet proven**: source-organization Actions jobs continue failing with zero recorded steps; the review/CI authorization process must unblock actual runner evidence before merge.

## LLVM-independent native acceptance gate

Use `-DORESLANG_PROXY_SLAB_HOST_ONLY=ON -DBUILD_TESTING=ON` to configure
a dedicated GCC/Clang C11+C++20 CTest build containing only the checked-in
host slab, real `ActorArena.c`, and exact-generation native scheduler
tests. This deliberately avoids a mandatory LLVM development dependency
for this *host-side* unit-test gate; normal Oreslang compiler builds are
unchanged. Execute all three registered CTests under both compilers and
run the direct ASan/UBSan workflow checks as separate required gates.
The actor test now rejects close while a retired request retains a pending
completion receipt, proves charges remain stable after rejection, and
rejects late callbacks after close. Neither these host tests nor the
standalone host-only CMake option establishes guest `.ores` lowering,
real HTTP/TCP callback quiescence, JDK25, or GraalVM parity.


## Explicit external-callback quiescence admission

The actor-owned native wrapper now requires a trusted host to call
`confirm_transport_quiescence()` on the correct logical actor turn **after**
stopping new physical I/O operations and joining any queued callbacks,
timers and wakers. Merely observing `live()==0`, recycling a ticket, or
retiring the scheduler does not authorize arena reclamation. Confirmation
is monotonic and disables subsequent request admission or callback pinning.
`close()` fails before confirmation; the destructor's scheduler-retired
cleanup also fail-stops if no confirmation was recorded. An outstanding
logical request (including a retired request with an unsettled completion
pin) prevents confirmation. The owning actor test exercises premature close
while logically empty, premature confirmation while a pin is pending,
foreign actor confirmation, rejection of admission after confirmation,
idempotent confirmation, and the terminal cleanup path.

**Trust boundary:** the confirmation call records a trusted host assertion;
it **does not inspect, join or revoke arbitrary external I/O callbacks**.
Transport and supervisor implementations must actually quiesce their own
queues before calling it. A malicious or faulty trusted host could lie, so
this alone is not evidence of real network callback safety. The guest
language cannot call this method through actor messages. No native guest
lowering, live HTTP/TCP integration or Java/GraalVM parity is claimed.


## Counted transport callback drain receipt (host integration layer)

`NativeTransportCallbackGate` adds explicit, allocation-free physical callback
accounting. A trusted I/O adapter **must obtain and retain one move-only
`Permit` before enqueueing each timer, waker, network callback or completion
that may touch actor arena memory**. Moving the permit with the asynchronous
completion envelope keeps it held across carrier migration, and releasing it
only after the callback's final access decrements the registered in-flight
count. A 32-bit atomic stop/count word serializes enqueue admission against
shutdown without a per-callback heap allocation, mutex or busy wait.

Shutdown invokes `stop_admission()`, preventing new permits, and only when
all registered permits are released can `try_drain()` produce a copyable,
non-default-constructible `DrainReceipt`. The receipt is bound to the exact
gate address, actor scheduler domain/slot/generation and globally unique slab
identity. `ActorOwnedProxyRequestSlab::confirm_transport_quiescence(receipt)`
checks this binding plus logical `live()==0` on the owning actor lane before
allowing `close()` or the retired destructor fallback. The old no-argument
trust-me assertion has been removed. Admission and callback pinning also stop
when the callback gate begins closing.

`native_proxy_transport_callback_gate_races` tests physical carrier
completion after logical shutdown, receipt rejection from another slab,
freeze/admission ordering and move-only permit semantics. Owner-wrapper tests
exercise premature and foreign confirmation, physical permits retained after
logical drain, replays, and ordinary close/destructor retirement.

**Scope limit:** the gate only counts callbacks whose transport adapters enroll
them. It does not automatically intercept epoll/kqueue/IOCP/HTTP callbacks,
join an external network reactor or prove a real HTTP/TCP shutdown. Any adapter
not yet wired to this gate remains out of scope; this is a host-side safety
substrate, not production guest networking or cross-backend conformance.


### Lock-free terminal admission hardening

The native callback gate uses one `std::atomic<uint32_t>` rather than a
mutex: the high bit means `stopped`, and the low 31 bits count admitted
physical callback permits. Admission is a CAS that fails once shutdown wins;
shutdown atomically sets the stop bit; each permit's final release decrements
the count with acquire/release ordering. `try_drain()` and receipt validation
require exactly the stop bit with zero outstanding permits. The low counter
cannot overflow into the stop bit, and a racing CAS cannot admit an uncounted
callback after freeze. The focused test includes 128 two-thread producer vs
freeze races in addition to a separately blocked in-flight callback. The
full native actor/repository test matrix remains gated on executing CI.

## Linux epoll callback ownership bridge

`NativeEpollTransportBridge` now wraps the checked-in bounded
`NativeEpollReactor` and an actor's `NativeTransportCallbackGate`. Each
accepted nonblocking one-shot `try_watch` acquires a move-only callback permit
**before** registering with epoll, stores it in a preallocated 64-slot table,
and transfers it to the terminal `CompletionLease` returned by `try_pop`.
Readiness, explicit cancellation, and reactor stop all generate terminal
records; **none releases an arena permit merely on event publication**.
A caller that forwards completion work to another carrier must move the
entire lease and keep it until the last actor memory access.

`stop()` closes the transport gate to new callback permits and joins the
epoll worker. Remaining `stopped` events still require consumption, and
`try_drain()` cannot succeed until every outstanding watch/completion lease
has released its permit. Destruction aborts if unconsumed terminal records
retain permits. No allocation occurs on the bridge's watch/pop/cancel path;
the Linux reactor's existing startup may create one native worker thread.

`native_epoll_transport_callback_retirement` exercises real Linux
nonblocking socketpair/epoll readiness, stop with an active watch, cancellation
with exact terminal delivery, and the drain barrier remaining closed while a
completion envelope is held. The actor wrapper also exercises request/pin
rejection as soon as the transport gate is frozen, eliminating a sampled
`accepting()` check-then-act race: the temporary permit itself is the
operation's admission linearization point, counted through the native write.

**Not yet complete:** The bridge is a new trusted host adapter, not automatic
retrofitting of every existing NIO consumer or production HTTP/TCP server.
Consumers must use this bridge rather than raw reactor methods for actor arena
traffic and retain leases through cross-thread mailbox handoff. Real guest
Oreslang compilation/execution and full actor suite are separate acceptance
gates. All native tests require exact-head CI or direct trusted execution.

## October 24 bounded epoll capacity and shutdown stress evidence

The additional `native_epoll_transport_capacity_stale_shutdown_stress`
CTest covers 64 simultaneous one-shot epoll watches at the preallocated
limit, rejection of a 65th watch, exact terminal `stopped` delivery and
permit retention for every outstanding completion, and generation changes
when a completed watch slot is reused. It also races transport stop against
watch admission 96 times and stop against concurrent cancellation 96 times.
No attempt to reconstruct a terminal record from a naked request ID is
accepted; the consumer must retain the entire move-only lease.

For the initial audit, the exact source blobs of the checked-in reactor,
transport gate, bridge and bridge test were reconstructed locally and Git
blob SHAs matched their PR-tree SHA values. The real socketpair/epoll bridge
test passed strict GCC 14.2 and Clang 17 builds, both ASan+UBSan variants,
GCC TSan and 80 repeated optimized runs per compiler. The additional
capacity/reuse/shutdown stress source blob was likewise compiled with
GCC/Clang strict C++20, ASan+UBSan and GCC TSan. The test now also
compares /proc/self/fd counts before and after all readiness and shutdown
iterations, verifying epoll/eventfd/socket handles are reclaimed. This is real execution of
the focused native source in a local subset, **not an exact full LLVM source
tree CMake/CTest or production guest actor runtime test**. Full CI,
independent review and protected-source provenance remain blocking gates.

## VM-backed actor-owned NIO completion admission

The native VM's existing `NativeEpollReactorManager` now supports a separate
trusted **C++ actor-owned completion mode** through
`NativeOresVMActorNio.hpp`. This mode is opt-in, irreversible for one NIO
backend, and mutually exclusive with the legacy C completion-record mode.
A legacy `ores_native_nio_try_pop()` cannot consume an actor-owned terminal
record and silently throw away its callback ownership. Invalid mode mixing
rejects without touching the completion queue.

`NativeActorNioCallbacks` holds one move-only
`NativeTransportCallbackGate::Permit` per admitted watch in a fixed
256-slot ledger (four reactor domains × 64 tickets). A watch is registered
with the VM's existing native reactor manager **only after** obtaining a
callback permit, and a short nonparking ledger mutex serializes registration
against completion consumption. On READY, CANCELLED or STOPPED,
`vm_nio_actor_try_pop()` returns the original readiness metadata plus a
process-unique nonwrapping `lease_id` and the source actor domain/slot/
generation/slab identity. The ledger **retains** its permit until trusted host
code calls `vm_nio_actor_acknowledge()` after the final physical actor-memory
access. A forged identity, mismatched ticket, stale lease, duplicate ack, or
a legacy completion poll cannot refund that permit. Host-only
`vm_nio_actor_stop()` joins the backend's reactor threads while still
requiring subsequent consumption and explicit acknowledgment of STOPPED
records. No guest actor can fabricate this receipt or access the C++ owner
ledger as a source-level method.

Every VM NIO API call acquires the existing backend retirement pin, so a
concurrent VM close cannot free the backend during that call. **The host
must also keep the VM and actor-owned callback gate alive across the entire
asynchronous operation**, fence producers, stop or cancel watches, pop all
terminal completions, acknowledge them, and only then retire/close the VM.
Teardown with unacknowledged actor-owned watches intentionally fail-stops
instead of dropping callback permits and freeing actor-owned memory.

This is native host/VM readiness integration only, **not** guest Oreslang
source lowering, a production HTTP request scheduler, or an externally
validated remote transport. Actor ownership remains a trusted-host API
boundary. The legacy C NIO behavior is unchanged for backends that select
legacy mode.

The `native_vm_actor_nio_owned_callbacks` regression exercises a real VM
and Linux pipe readiness, foreign/forged/duplicate lease rejection,
cross-carrier acknowledgement, cancellation, STOPPED completion and
fail-closed legacy mode mixing. Register it in the opt-in host-only and
ordinary Linux native CMake test matrix, GCC and Clang, plus ASan/UBSan.


## Bridge destruction versus externally held completion envelopes

An audit found that `NativeEpollTransportBridge::try_pop()` moves the
`NativeTransportCallbackGate::Permit` into a downstream completion envelope
and clears its internal slot. The prior destructor checked only the now-empty
slot table; it could therefore return while another carrier still possessed an
external callback permit. The bridge destructor now additionally requires the
entire associated transport gate to be terminally stopped **and drained**
(`try_drain()`), failing closed if any callback ownership is still in flight.
This applies to all registered users of that gate, not just the bridge's own
slot table. The host must retire all completion envelopes before destroying
the bridge and must keep the gate alive until the final release.

The focused `native_epoll_bridge_external_handoff_failstop` CTest verifies
ordinary drained shutdown and uses a separate process with real Linux
socketpair/epoll readiness to prove that destroying a bridge with a moved-out
completion envelope terminates with SIGABRT rather than silently succeeding.
A parent process verifies the expected failure and reports success. The
regression is registered in the source host-only and normal Linux CMake CTest
configurations and the exact-head GCC/Clang ASan+UBSan workflow.

**Evidence boundary:** The exact header and test sources were checked against
Git blob identity and compiled and executed locally with GCC/Clang strict
C++20, optimized repetition, ASan and UBSan. This is focused host-runtime
proof only; an executing source CI runner is still required for the complete
LLVM actor and VM integration suite. No private compiler tree was mirrored.


### FD loan lifetime and terminal-envelope identity (October 24, 2026)

The actor-owned VM NIO completion ledger now **retains exclusive ownership of
its watched FD until terminal completion acknowledgment**, not merely until
`NativeEpollReactorManager::try_pop()` releases the kernel readiness slot.
An outstanding delivered completion therefore prevents a second actor, slab,
or generation from registering that same numeric descriptor through the
actor-owned interface. Only the exact acknowledged terminal completion can
release the counted callback permit and make the descriptor eligible again.

The ledger also preserves the original FD and full delivered terminal record.
Acknowledge refuses altered FD, readiness events, terminal outcome, watch,
lease identity, or actor owner metadata. The host retains responsibility for
not closing/recycling the numeric FD before its terminal acknowledgment, and
for guaranteeing that acknowledgment follows the consumer's **last physical
access**. This does not make copyable host metadata a guest capability or
retroactively protect legacy C NIO mode. Native actor-owned VM tests exercise
rejection of FD reuse before acknowledgment, positive rearm afterward, and
hostile terminal-envelope modification.
