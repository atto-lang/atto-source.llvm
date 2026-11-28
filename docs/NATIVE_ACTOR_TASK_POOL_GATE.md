# Native actor Task worker-pool gate

Status: **fail-closed precursor; no LLVM guest actor-task execution claim**.

An AttoLang `task { ... }` created inside an actor must execute on the
owning actor incarnation's pool, with every resume serialized through its
actor scheduler and no execution on supervisor CONTROL or a foreign Future
producer. The current native `SourceTaskRuntime` scalar bridge opens its own
single synthetic actor scheduler; that is NOT an actor-owned scheduler.

## Active safeguards

1. `Parser.cpp` rejects `let Task<int> ...` inside the native scalar
   actor `on_message(int)` subset with an actor-scheduler-specific diagnostic.
   It cannot publish an AOT artifact that silently switches to the unrelated
   legacy Task pool.
2. `oreslang_source_task_start_i64_checked` rejects an authenticated actor
   carrier (DATA, WAKE or CONTROL callback) with
   `ORES_SOURCE_TASK_ACTOR_POOL_UNSUPPORTED` before allocating a Task or
   submitting to the synthetic pool. This is a runtime defense in depth for
   hand-written host/FFI call paths.
3. `source_task_i64_runtime_test.c` creates a real native actor callback,
   attempts the legacy Task ABI, verifies explicit rejection, then drains and
   physically retires the actor scheduler.

The eight unchanged native C and header/test blobs have an executed
GCC O2, Clang O2, and GCC ASan/UBSan **host-only** proof on the private CI
carrier run #43312320747 (2026-11-12). That receipt predates the additional
native parser fixture; it is not an exact-HEAD LLVM parser or compiler proof.

## Compiler-negative gate

`tests/fixtures/task-v1-negative/actor-task-no-pool.atto` must fail before
an executable artifact is published. The existing
`scripts/test-source-task-atto-aot.sh` exact-source O0/O2 gate checks the
expected precise diagnostic alongside other native Task negative cases.

## Required before enabling source actor Tasks

- Replace single synthetic `Task<int>` ownership with a per-actor
  incarnation task record and authenticated scheduler admission.
- Add bounded actor-owned continuation slots (including STOP cleanup lane).
- Implement async suspend/resume without parking an actor carrier and route
  external Future completion through scheduler wake admission only.
- Transfer immutable/owned captures with checked Send authority and enforce
  private/untrusted quotas through nested tasks.
- Seal admission with actor STOP; prove cancellation, cleanup and physical
  retirement before `get_done()`.
- Execute the same `.atto` guest conformance fixtures as the Java backend
  with compiler digests, exact head/tree SHAs, real worker-pool receipts,
  concurrency stress and sanitizer evidence.

The host-only refusal is a safety property, **not functional parity**.
