# State-region primitive (experimental)
This is a **standalone native C++20 prototype**, not integrated with LLVM compiler IR,
actor scheduler, or production reclamation. It uses no tracing collector and has
no hidden allocation or synchronization, but it does not manage actual heap arenas.

Build/test:

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
  contracts/state_regions/test_state_region.cpp -o /tmp/ores-state-region
/tmp/ores-state-region
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  contracts/state_regions/test_state_region.cpp -o /tmp/ores-state-region-asan
/tmp/ores-state-region-asan
```

The local baseline compile/run was PASS for 100,000 increments with zero counted
C++ operator-new allocations, but the counter does not cover malloc, OS mappings,
or every native allocation API. Assertions must remain enabled in this test.
The same test was also executed locally with AddressSanitizer and UBSan and passed. No GitHub Actions run is claimed.

The prototype is **not thread safe**. All acquire/release/pin/retire state
transitions require external serialization by the actor runtime or lock;
using it directly from concurrent threads is a data race. The primitive
tracks counts rather than individual task identities and does NOT prevent a
wrong task from releasing another task's capability. Both limitations must be
closed with task/capability-identity checks before production admission.

A completed native region backend must additionally enforce non-escaping
borrows at compile time, pin actual callbacks/FFI/continuations, reclaim
owned arenas only after real retirement, and verify mutation allocation +
latency under load. The region struct itself does not free memory.

## 2026-10-30 hardening
The test now uses `CHECK` (which cannot be compiled away by `NDEBUG`) instead of `assert`. Reconstructed exact test logic locally passed optimized, `-DNDEBUG -Werror`, and ASan/UBSan runs. This is developer-host smoke evidence; exact-head GitHub Actions execution has not been established. Do not interpret zero-step workflow failures as test failures or as evidence of exhausted Actions minutes.

Private source export must remain disabled until the source repository's independently reviewed protected-main policy authorizes the exact destination identity and exact source snapshot. The current approved-destinations file contains no destinations. Runner capacity in another org is not transfer authorization. Source-free independent tests can be run elsewhere without copying the private Git tree.


## Task-bound production bridge

The draft now also contains `OresTaskBoundStateRegion.h`. It composes the
existing fixed-capacity `OresStdSyncAdmission` reader/writer gate with the
native execution identity model rather than inventing a second scheduler lock.

Each state-region ticket is bound to:
- logical `runtime_domain + execution`;
- generation-checked execution-store `slot + generation`;
- the exact region object/generation through the underlying scoped ticket.

The preferred binder calls `ores_native_execution_context_store_get` and
verifies that the live generation-checked record carries the expected logical
execution ID. Mismatched IDs, stale handles, foreign tasks, foreign regions,
double release and suspension with a live ticket fail closed.

The runtime gate remains bounded/nonparking: it has a fixed 64-ticket table and
does not allocate per request. ExecutionContextStore itself is separately
allocated runtime infrastructure; this change does not claim that opening that
store is allocation-free. The state-region mutation/admission fast path adds no
new heap allocation.

The compiler CFG now has optional region-backed loan provenance containing
region ID/generation and logical execution/store generation. A region-backed
release must match the exact provenance; a local lexical loan cannot be
laundered into a region loan. This metadata is a compiler safety foundation,
not source syntax lowering.

## CI evidence boundary

Source-org exact-head LLVM workflows continue to fail before runner execution
with zero recorded steps. No private compiler/native CTest pass is claimed for
the new task-bound header or CFG changes.

A separately authored source-free equivalent ran successfully on a real funded
GitHub-hosted runner in `honeypot-r-us-test/hnpt-e2e`:
- branch: `test/state-region-source-free-canary-20261030`
- exact head: `73c3fed5939a1e7158a9c9c625232118c261425e`
- run: `41003990044`
- Python task-identity state-region model: PASS
- optimized C11 `-DNDEBUG -Werror` canary: PASS
- ASan + UBSan canary: PASS

This proves the independent model and alternate runner capacity only. The LLVM
private-CI approved-destination file currently contains no destination entries,
so no private LLVM source was copied to the carrier.
