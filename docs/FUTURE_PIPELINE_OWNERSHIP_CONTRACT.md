# LLVM Future-aware pipeline ownership contract (v1)

This file pins the **compiler proof contract** required for parity with Java/Graal PR #2048. It is intentionally not a claim that general source Future pipelines already execute on LLVM.

## Per-edge ownership

A checked Future-aware value pipeline has exactly three input ownership states:

- `BorrowedUpstreamSharedPayload`: an ordinary root `source |> stage()` observes the producer Future without consuming it. Its payload must prove **Send + Share**. Cancelling downstream work must not revoke the producer Future.
- `ConsumedUpstreamOwnedPayload`: `rt take source |> stage()` transfers the root Future. Its payload must prove **Send** but need not prove Share. The eventual native lowering may revoke a still-pending root on pipeline cancellation/admission failure.
- `OwnedPipelineValue`: every later deferred stage receives a value already owned by the pipeline. It requires **Send**, not Share.

A root may never be classified as `OwnedPipelineValue`; a non-root deferred edge may never be reclassified as borrowed or consumed. Adjacent deferred stage types must compose exactly in this initial fail-closed proof layer.

## Effect provenance

Ownership and effect provenance are orthogonal. `rt take` cannot turn an opaque or forwarded Future into `CompleteStatic`.

`CompleteStatic` is established only by exact producer identity. An `UpstreamFutureUnknown` prefix remains unknown through every later stage. Each plan records the exact prior effect row, stage row, and cumulative row. The cumulative row must equal `prior ∪ stage`, and each non-root prior row must exactly equal the preceding stage's cumulative row. Effects may neither disappear nor be forged into a checked plan.

Source exceptional effect execution remains fail-closed until native throw/raise/panic continuation unwinding exists. Executable admission checks the **cumulative** effect row so an exceptional effect from an earlier stage cannot be overlooked by a later pure stage.

## Future flattening

A stage that flattens one Future layer must own exactly that stage-created Future for cancellation **and** carry `FreshCheckedProducer` provenance. A forwarded/borrowed Future cannot be relabelled as owned merely because the surrounding pipeline root was consumed. A scalar stage carries neither child-Future ownership nor Future provenance.

## Proof identity and publication

A staged plan binds two compiler-only identities:

- the exact resolved declaration identity;
- the exact checked value-pipe AST-node identity.

Ownership admission must match both identities **and** the planned input ownership mode. A bare boolean admission result is insufficient because admission for one call must never authorize a rewritten or rebound call.

Publication is atomic across the staged chain, matching the Java/Graal `CheckedFuturePipe` boundary: if any edge fails ownership admission, no earlier prefix receives executable authorization. Identity mismatches are hard errors rather than partial publication.

## Current LLVM boundary

As of this contract landing, the native parser still deliberately rejects general `async` declarations and generic `Future<T>` source values because the required continuation representation/lowering is not implemented. That fail-closed behavior stays in place.

The next implementation slice is to connect this contract to a native Future continuation representation and parser/type/ownership nodes, then prove real `.ores` source -> LLVM -> linked execution. Until that exists, host-only contract tests are semantic hardening, not general Future-pipeline parity.
