# Native source-defined producer → consumer actor pipe (draft)

This is a **narrow executable LLVM compiler/runtime slice**. Unlike the
trusted accelerator profile that maps fixed actor roles to host callbacks,
both producer and consumer `on_message(int): int` bodies in
`tests/fixtures/source-actor-pipe.ores` are parsed, ownership-checked, and
compiled into LLVM functions. Their runtime actors hold independent private
i64 heaps and scheduler-owned bounded FIFO mailboxes.

```ores
const p = spawn Producer();
const c = spawn Consumer();
p pipe c;
p.send(1);
p.send(2);
stop p;
stop c;
```

## Admission and memory boundaries

- This milestone supports **exactly two** owned native source actors, one
  linear one-way `int` result-to-`int` message edge, and a trusted
  zero-argument `int main()`; no multi-stage arbitrary graph lowering yet.
- The producer's generated `on_message` result is passed by value to
  the consumer's generated `on_message`. No actor fields, pointers,
  borrowed references, heap handles, actor refs, or mutable aliases cross
  the mailbox boundary.
- The compiler rejects unknown/self-connected endpoints, multiple links,
  direct consumer sends, unsafe late graph connection, missing stops,
  consumer-before-producer stop, and unsupported `await`/effects before
  emitting LLVM IR.
- The native source runtime bounds forwarded-but-not-acknowledged messages
  at 128, and a connected producer must reserve downstream credit before
  any upstream admission. Only this source may write the downstream FIFO,
  so a reserved send cannot overfill the sink. Rejections are nonblocking
  through `oreslang_source_actor_try_send()`; the existing guest
  `worker.send(int)` remains a void effect and **traps on overload**.
  A typed guest `try_send`/`send_async` and wait-free retry suspension
  is still pending.
- Producer `stop` is a **host main-only** drain/finalization barrier.
  It waits for both the source and consumer accepted messages to retire,
  releases the one-way link, and then permits the consumer's stop and heap
  finalization. Never use this synchronous drain from a runtime actor lane.
- The executor runs distinct per-actor native scheduler callbacks, not a
  shared synchronized user-code thread. No source-defined `await`,
  scheduler-owned continuation frames, GPU driver events, dynamic
  StreamOperator ports, supervisor restarts, or network IO are released.

## Required gates

- [ ] Exact-source funded GitHub CI builds real LLVM parser/checker/codegen,
  emits native LLVM IR, links Clang/C11 actor runtime, and executes the
  positive producer/consumer fixture with expected producer final result
  `6` and consumer final result `8`.
- [ ] Existing source actor lifecycle fixtures and negative no-IR
  ownership/admission cases pass.
- [ ] Current-head full native CMake/CTest and ASan/UBSan suites.
- [ ] Direct native backpressure and cancellation stress, actor panics,
  supervisor CONTROL, lost wakeups, abort-free typed guest overload API.
- [ ] Matching JVM/GraalVM source actor execution and typed graph parity.
- [ ] General source `ActorStream<I,O>` and `await` lowering with
  suspension-aware ownership checker and scheduler continuations.

Do not merge or claim production actor-pipe readiness based on a single
targeted exact-source test run.
