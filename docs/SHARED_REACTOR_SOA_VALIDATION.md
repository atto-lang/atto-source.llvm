# Shared reactor / SoA validation — 2026-10-29

PASS locally: GCC 13.3, C++17 -O2, strict warnings, AddressSanitizer + UndefinedBehaviorSanitizer; real Linux eventfd readiness and concurrent producers. Five additional repeated runs passed.

LeakSanitizer cannot inspect /proc in this execution environment; local runs used ASAN_OPTIONS=detect_leaks=0. Address and undefined-behavior instrumentation remained enabled. Leak detection and Clang are staged in CI, not claimed locally. CMake/CTest was unavailable; the exact target sources were compiled directly.

Host components remain opt-in. Existing runtime/CLI defaults and guest feature admission are not switched to these pools. No compiler-backed five-backend guest parity or hard-real-time guarantee is claimed. The native and Java tests cover bounded saturation, 8,000 concurrent submissions, one polling owner, actual readiness, same-thread polling/execution in one-worker mode, hot-ready-queue I/O fairness, explicit growth, ordinary task failure, self-join rejection and accepted-work draining.

.NET uses the same scenario set but still needs actual SDK10 execution. WASM has admission parity only. Production actor/CONTROL/isolate integration, cancellation/kernel retirement, flags-2-env CLI integration, full backend regression and hardware performance gates remain outstanding.

## Exact component Git blobs

- `src/main/cpp/oreslang/runtime/SharedReactorPool.hpp`: `90542d00e55d98e1d1ce1ea9510c2d76e7e5ea68`
- `tests/shared_reactor_pool_test.cpp`: `05570efaa8abebeded0d9700aeb511630409606a`
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

## Incremental wake-gating hardening — 2026-11-03

Hot-queue submissions avoid calling the reactor wake function when the single polling lease is absent. The polling lease is acquired before the poller's ready-queue check; active-lease submissions still wake it after publication. Added a deterministic saturation regression: while the only executor is blocked in user work, 1024 accepted queue entries do not trigger reactor wakeups. An ordinary shutdown continues to wake an active reactor.

Exact-file Git blobs verified locally. C++17 strict `-Wall -Wextra -Werror` with GCC and Clang, each at O1 and O2, ASan+UBSan, five executions per combination (20/20 total), PASS. `ASAN_OPTIONS=detect_leaks=0` (LeakSanitizer not established). Full CMake, compiler guest, and production bridge untested.

These results are standalone host-substrate evidence only. The PR remains draft and opt-in; runtime wiring, actor authority, CONTROL priority, cancellation generation fencing, and total-process thread measurements are still required. Remote zero-step workflow failures are not execution evidence.

## Poll failure containment, fail-closed drain and runner capacity — 2026-11-03

A faulting reactor must not leave a worker spinning or lose previously accepted entries: once an exception seals admission, workers drain already-admitted work but do not re-enter polling. C++17 no longer requires `poll` to be `noexcept`; unexpected exceptions are caught at the scheduler boundary while `wake` still must be nonblocking and `noexcept`.

Exact LLVM native implementation/test blobs verified by `git hash-object` before local execution. GCC/Clang × O1/O2 with `-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined` each passed 5/5 executions (20/20 total). Fault-injected reactor poll failure sealed admission and drained 128 accepted tasks without repoll, one failure total. LeakSanitizer disabled; full LLVM compiler and source guest execution NOT_EXECUTED. The earlier `dirty` merge conflict was resolved by an exact-tree merge of current `main` and seven verified PR-owned blobs; GitHub subsequently returned `mergeable=true`, `mergeable_state=unstable`. The latter denotes outstanding status checks, not a new source conflict.

GitHub focused CI jobs in the source organization have previously failed without executed steps; these are not test failures. A separately authorized **source-free** GitHub Actions runner canary succeeded in private alternate-org carrier `opto-sync-test/go-java-validation`, run https://github.com/opto-sync-test/go-java-validation/actions/runs/41589765720, commit `7705a21d567f9637dbe035c69cd9be8dfbd1e8e2`. This demonstrates runner capacity only and does not certify or authorize exporting private source.

The protected-source repository policies have no active approval for cross-organization source mirroring; no source, tree, or runtime test fixture was transferred to the alternate org. Real cross-org source-bearing CI remains BLOCKED by governance. The opt-in pool and draft PR must not be treated as production 2-thread startup or five-backend guest parity proof.
