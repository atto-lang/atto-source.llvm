# LLVM Future-aware pipeline ownership contract (v1)

This file pins the **compiler proof contract** required for parity with Java/Graal PR #2048. It is intentionally not a claim that general source Future pipelines already execute on LLVM.

## Per-edge ownership

A checked Future-aware value pipeline has exactly three input ownership states:

- `BorrowedUpstreamSharedPayload`: an ordinary root `source |> stage()` observes the producer Future without consuming it. Its payload must prove **Send + Share**. Cancelling downstream work must not revoke the producer Future.
- `ConsumedUpstreamOwnedPayload`: `rt take source |> stage()` transfers the root Future. Its payload must prove **Send** but need not prove Share. The eventual native lowering may revoke a still-pending root on pipeline cancellation/admission failure.
- `OwnedPipelineValue`: every later deferred stage receives a value already owned by the pipeline. It requires **Send**, not Share.

A root may never be classified as `OwnedPipelineValue`; a non-root deferred edge may never be reclassified as borrowed or consumed.

## Effect provenance

Ownership and effect provenance are orthogonal. `rt take` cannot turn an opaque or forwarded Future into `CompleteStatic`.

`CompleteStatic` is established only by exact producer identity. An `UpstreamFutureUnknown` prefix remains unknown through every later stage. Cumulative effect rows are monotonic and cannot silently drop earlier or current stage effects.

Source exceptional effect execution remains fail-closed until native throw/raise/panic continuation unwinding exists. Plans may retain those rows without granting execution authority.

## Future flattening

A stage that flattens one Future layer must own exactly that stage-created Future for cancellation. A scalar stage owns no child Future. Forwarded/borrowed Futures must not be relabelled as owned merely because the surrounding pipeline is consumed.

## Proof publication

The type checker may stage a plan, but executable authority may be published only after ownership admission succeeds for the same exact resolved declaration/AST proof. A failed move/use-after-take check must leave no executable pipeline proof behind.

## Current LLVM boundary

As of this contract landing, the native parser still deliberately rejects general `async` declarations and generic `Future<T>` source values because the required continuation representation/lowering is not implemented. That fail-closed behavior stays in place.

The next implementation slice is to connect this contract to a native Future continuation representation and parser/type/ownership nodes, then prove real `.ores` source -> LLVM -> linked execution. Until that exists, host-only contract tests are semantic hardening, not general Future-pipeline parity.
