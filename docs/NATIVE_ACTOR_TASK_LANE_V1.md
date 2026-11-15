# Native actor-owned Task lane — host runtime stage

Status: **native host scheduler primitive implemented; compiler-produced AttoLang actor `task { }` and nonparking `await` NOT VERIFIED**. This is stacked on the legacy scalar Task actor-pool rejection change (LLVM PR #1715). Five-backend conformance still requires the guest execution gate in oreslang-platform-conformance PR #712.

## Runtime ownership

A source Task admitted from an authenticated native actor's physical DATA/TASK/WAKE turn must use the **existing NativeActorScheduler**, not `SourceTaskRuntime`'s separate scalar worker, CONTROL, an I/O producer, or another actor's carrier.

`ores_native_actor_scheduler_try_schedule_task(s, owner, handle)` is trusted host-only. It succeeds only when the caller owns the exact actor generation and live physical single-writer claim; CONTROL, foreign/Future/root threads, a different actor, and stopped actors are rejected. Admission is immediate and bounded; callback execution is deferred. `handle` is a runtime-managed logical continuation ID, **not** a raw source-visible pointer or a native actor capability.

The scheduler has 8 preallocated TASK queue slots per actor *when task callbacks are configured*. The TASK lane is separate from user DATA mail and the existing WAKE escrow / CONTROL lanes. Priority order is CONTROL > WAKE > TASK > DATA. A ready native TASK is executed under the same actor scheduling lease, carrier pool, allocation domain and generation as an ordinary DATA callback. Nested TASK admission inherits physical actor identity, not supervisor identity. No per-task/pseudo-actor OS pool is created.

## STOP, physical quiescence and safety

An option must supply both `on_task` and `on_task_cancel`, or neither. This makes it impossible to silently drop an accepted task handle during STOP. Once STOP wins, new Task admission is forbidden. A previously queued handle whose dispatch starts after STOP is passed **only** to `on_task_cancel` for runtime-owned cleanup (not guest execution); the caller must reclaim or settle the handle. Pending TASK callbacks count toward native scheduler drain and actor `wait_done()`. Physical drain is stronger than Future terminal settlement.

A task already running when STOP wins still requires **compiler-inserted cancellation safepoints and resumable cleanup lowering**. This host callback API does not preempt an active native guest turn and does not by itself implement TaskCancellation, structured `task scope`, parent/child join, actor state critical-read/write, or completed Future delivery. Production code must reject source `task { }` when those integration gates are unavailable.

A foreign Future producer may not call `try_schedule_task` directly. It must first publish an authenticated native continuation WAKE ticket to the owning actor; the actor's WAKE callback may then enqueue an already-validated Task handle. Future implementation should retain the exact incarnation/continuation generation and guarantee cleanup-only resume once STOP seals admission.

## Executed native-host evidence

- [Exact-blob private native run #43324773459](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/43324773459): GCC O2, Clang O2, GCC ASan/UBSan; new actor TASK lane + baseline actor scheduler + legacy scalar Task regression passed.
- [Expanded exact-blob run #43325099721](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/43325099721): all preceding tests plus STOP-sealed, actor CONTROL isolation, typed CONTROL failure, pthread creation failure injection, and O(1) ready-queue validation/stress passed.

Both jobs verified the exact Git SHA of each copied C source, header, and test file before execution. They validate the **tested blobs**, not a full LLVM repository tree, compiler-produced guest program, ABI cross-backend parity, or hard-real-time certification. This CMake/CTest registration is newer than those receipts and still needs a full source checkout build.

## Remaining executable guest release gates

1. Lower compiler-checked actor TaskExpr to an owned native continuation table with task-generation and parent-cancellation identity, not `SourceTaskRuntime.start_i64`.
2. Implement nonparking Future `await` and authenticated producer-to-actor WAKE escrow, verifying identity and reactor isolation after every resume.
3. Enforce STOP cleanup-only execution for **active** guest tasks at cooperation boundaries, retain physical pending-child quiescence before actor DONE, and share UNTRUSTED quotas.
4. Verify the same `.atto` nested task, negative `self`/mailbox capture, cancellation and cross-actor fixtures on Java/GraalVM and native LLVM.
5. Run the complete native regression and sanitizer matrix at the exact final LLVM source-head SHA with compiler-produced guest receipts; keep this PR draft until then.
