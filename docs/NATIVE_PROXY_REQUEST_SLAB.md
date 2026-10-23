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
