# GPU actor host-turn bridge v0

**Evidence tier: native host-control integration ONLY. No GPU actor guest lowering and no physical GPU work.**

This native-only bridge composes three existing production source components rather than creating a second mailbox or a second GPU authority:

- `NativeActorTurnCredits`: one exclusive actor claim per turn, no concurrent mutation of one actor
- `GpuBufferFence`: per-allocation submitted, quiescent, poisoned, and retired ledger
- `SupervisorFailureLane`: per-incarnation CONTROL reservation and bounded terminal failure publication

See `src/main/cpp/oreslang/runtime/GpuActorHostTurnBridgeV0.hpp` and `tests/gpu_actor_host_turn_bridge_v0_test.cpp`.

## Host state machine

```text
READY -- prepare(delta, checked-i64) --> PREPARED
PREPARED -- begin_enqueue --> ENQUEUE_IN_PROGRESS
ENQUEUE_IN_PROGRESS -- mark_submitted --> SUBMITTED
SUBMITTED -- trusted provider event complete --> READY (commit staged value)
SUBMITTED -- provider event failure --> CLOSED + HostFailure CONTROL
PREPARED -- cancel --> CLOSED + Cancel CONTROL (abort_unsubmitted)
ENQUEUE_IN_PROGRESS -- cancel/device loss --> STOP_PENDING (must resolve enqueue)
STOP_PENDING -- mark_submitted --> POISONED_PENDING_EVENT
STOP_PENDING -- abort_enqueue (definite not enqueued) --> CLOSED + CONTROL
POISONED_PENDING_EVENT -- confirmed provider event quiescent --> CLOSED + CONTROL
READY -- graceful provider-backed release --> CLOSED (no failure CONTROL)
```

A close or device-loss notification occurring between the start and acknowledgment of a provider enqueue **must never** infer the enqueue failed. Until the provider confirms it definitely did not enqueue the command, physical buffers remain in-flight/quarantined. A submitted or possibly submitted command cannot use `abort_unsubmitted`.

An event is associated with the actor incarnation and turn sequence. A late completion from an old turn is denied. The host-side pending turn holds a native unique buffer ticket; state is not committed on failure, cancellation, or device-loss. A live actor bridge cannot be destroyed without a deliberate terminal/retirement transition; destruction of an unresolved bridge fails stop. All 1,024 actor host-control fixtures exercise graceful retirement and supervisor reservation release.

## Trust boundary (important)

The methods `mark_submitted`, `finish_after_provider_event`, `abort_enqueue`, `acknowledge_cancelled_provider_event`, and `acknowledge_backend_buffer_release` are **trusted-host integration seams**. They accept asserted enqueue outcomes or booleans from native code. They do **not** authenticate GPU events, queue authority, kernel binaries, physical allocation handles, buffer transfer, or device drivers. No guest can gain these entrypoints through language syntax in this proposal.

The real provider adapter must bind these seams to a provider-issued opaque event identity and trusted device generation; it must perform exact device/tenant/actor authority validation, check physical release success before signaling quiescence, and deliver supervisor CONTROL without reaching into the actor heap. `GpuBufferFence` itself is a descriptor ledger, **not a physical provider**.

## Executed CI

Private exact-Git-blob carrier: [run #42916949277](https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42916949277), using production header files and the executable test, GCC strict flags with ASan/UBSan, and Clang strict flags. Both GCC/ASan/UBSan and Clang report 6,257 executed checks, including 1,024 logical actors, 4,096 host-simulated turns, and 1,024 clean retirements, concurrent claim, checked overflow, rejection of stale/fake turn IDs, cancellation before and during enqueue, post-submission quiescence, failed enqueue, device loss affecting a cohort, and supervisor failure routing.

This does **not** prove execution of a GPU kernel, a compiled `.atto` actor, a physically authenticated fence, or five-backend parity. It is intentionally a narrow G1 host-control milestone; do not promote to production GPU actor support from this result.

## Next real runtime work

1. Add a provider-owned opaque submission/event handle (not a Boolean) to the host bridge and couple begin/confirm submission to one trusted physical enqueue API.
2. Connect the typed Java/LLVM GPU actor IR and its checked ownership/effect rows to provider authority, without unverified CPU fallback.
3. Run the existing OpenCL native kernel prerequisite on a real GPU runner, followed by an actor guest end-to-end test, with device model/driver, source/build SHA, kernel binary hash, event proof and independent result oracle.
4. Enforce loss and cancellation quarantine, supervisor tree recovery, cross-actor alias rejection and multi-backend conformance.
