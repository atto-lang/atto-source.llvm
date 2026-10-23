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
