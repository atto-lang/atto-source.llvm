# Shared reactor / SoA validation — 2026-10-29

PASS locally: GCC 13.3, C++17 -O2, strict warnings, AddressSanitizer + UndefinedBehaviorSanitizer; real Linux eventfd readiness and concurrent producers. Five additional repeated runs passed.

LeakSanitizer cannot inspect /proc in this execution environment; local runs used ASAN_OPTIONS=detect_leaks=0. Address and undefined-behavior instrumentation remained enabled. Leak detection and Clang are staged in CI, not claimed locally. CMake/CTest was unavailable; the exact target sources were compiled directly.

Host components remain opt-in. Existing runtime/CLI defaults and guest feature admission are not switched to these pools. No compiler-backed five-backend guest parity or hard-real-time guarantee is claimed. The native and Java tests cover bounded saturation, 8,000 concurrent submissions, one polling owner, actual readiness, same-thread polling/execution in one-worker mode, hot-ready-queue I/O fairness, explicit growth, ordinary task failure, self-join rejection and accepted-work draining.

.NET uses the same scenario set but still needs actual SDK10 execution. WASM has admission parity only. Production actor/CONTROL/isolate integration, cancellation/kernel retirement, flags-2-env CLI integration, full backend regression and hardware performance gates remain outstanding.

## Exact component Git blobs

- `src/main/cpp/oreslang/runtime/SharedReactorPool.hpp`: `260b616d6728f04cc06a8111dda8dd43305d8ebe`
- `tests/shared_reactor_pool_test.cpp`: `c12f2c6895bb81960be921edc409f8c96fdb5841`
- `tests/shared_worker_layout_benchmark.cpp`: `753dd4769e6160ceb03def0f8a99cd94bbc2c6a5`

## Counter microbenchmark (local shared host)

GCC13.3 -O2, five samples, median nanoseconds per atomic counter update. No CPU pinning; these timings are illustrative, not release acceptance or scheduler throughput.

| Writers | AoS | Dense SoA | Padded SoA |
| --- | --- | --- | --- |
| 1 | 1.979 | 3.153 | 1.901 |
| 2 | 1.116 | 6.772 | 1.197 |
| 3 | 3.607 | 9.809 | 0.793 |
| 4 | 6.076 | 9.863 | 0.566 |

Padded SoA was not uniformly faster than AoS (two writers). Dense SoA alone is not a maximum-performance guarantee. End-to-end latency, idle CPU and allocation benchmarks remain required.
