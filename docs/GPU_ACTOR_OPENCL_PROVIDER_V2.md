# GPU actor OpenCL provider V2 — native physical adapter milestone

**Status: executable native OpenCL actor-to-kernel adapter, with host-mock CI verification. Not a compiled AttoLang source actor and not hardware-attested on the available runner.**

This draft stacks on the host quarantine/provider V1 proposal (#1652, itself stacked on #1650). Rather than invent a new OpenCL runtime, `GpuActorOpenClProviderV2.hpp` adapts the existing `OpenClSystemApi` and `OpenClProvider` to `ProviderPortV1`. It consumes an admitted typed immutable DOP f32 kernel and exact-layout host input, creating the two real buffer fences and OpenCL provider queue claims.

A physical device implementation is selected only through `OpenClSystemApi::try_create_gpu()`. `require_physical_opencl_gpu()` throws if there is no compatible GPU. CPU OpenCL platforms are rejected. Device and queue metadata must agree with the actor binding. The external GPU provider still needs independent attestation of device-generation freshness and tenant capabilities before it can be used with untrusted actors.

## Native dispatch and completion

A trusted host initiates an actor turn. The V1 registry verifies the actor incarnation and turn, and V2 invokes `OpenClProvider::begin()`, which reserves device quotas, allocates input/output buffers and submits an asynchronous upload with a native OpenCL event. The existing provider then advances only after native event status:

1. Uploaded input completion → checked kernel enqueue
2. Kernel completion → checked output download enqueue
3. Download completion → physical event/buffer release, output bytes available
4. V2 locally issued event observation = `CompleteOk` → V1 atomically publishes staged actor scalar state
5. Provider program/kernel release must succeed before the actor and supervisor reservation retire

No state publication occurs on initial upload, kernel submission, failed download, cancellation, or unverified device completion. V2 rejects event replay once the operation is retired. The provider's native event observation remains within the trusted runtime boundary; a mock Api is only a testing dependency.

Native OpenCL status queries are fail-closed: when clGetEventInfo fails, the SystemApi returns an unknown/pending event instead of treating the failed query as a terminal command. Negative tests confirm invalid-event and other failed queries cannot authorize buffer release.

Device loss is not physical quiescence. The actor reports HostFailure CONTROL. The existing OpenCL provider must prove queue quiescence and retire the physical allocations through `recover_device_loss` before V2 reports `CompleteFailed` to the supervising actor registry. The actor never commits staged state on recovered failure.

## Fail-closed unknown enqueue

An exception from `OpenClProvider::begin()` can occur after the underlying API accepted a command. V2 therefore does not fabricate `DefinitelyNotSubmitted`; its `reconcile_unknown` returns Unknown and process-root quarantine retains the provider and actor ledger.

**Production blocker:** OpenClProvider must expose a provider-owned, authenticated recovery receipt for ambiguous `begin` attempts. Until then, an ambiguous enqueue can consume a quarantine slot indefinitely. This is safer than premature free but not acceptable as a complete production recovery mechanism. The in-process process-root is not crash durable and does not prove GPU reset safety.

## Executed tests

Exact private Git blob mirror and CI: https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42949702555

- GCC C++17 strict warnings with AddressSanitizer and UndefinedBehaviorSanitizer: pass, 65 assertions
- Independent Clang C++17 strict warnings: pass, 65 assertions
- The repository's preexisting `opencl_provider_g3_test.cpp` compiled and executed under GCC -Werror + ASan/UBSan against the exact mirrored OpenCL provider sources: pass
- Real production `OpenClProvider` and `OpenClSystemApi` source compiled; injected OpenCL Api drives mock upload/kernel/download, numerical oracle, actor state publication, supervisor CONTROL, native release, and device-loss recovery
- Native physical-GPU integration probe compiled successfully
- Physical probe attempted; compatible OpenCL GPU runtime/device absent → `GPU_ACTOR_V2_NO_GPU_RUNTIME`, **NOT_EXECUTED**, no CPU fallback

No compiled `.atto` actor guest, physical GPU event/kernel, AOT guest, real actor mailbox, full repository CTest or five-backend parity was verified.

## Remaining P0/P1 work

- Close legacy V0 trusted-native raw Boolean completion bypasses without breaking existing testing; make physical provider capabilities explicitly privileged and unavailable through guest FFI.
- Add provider-backed receipt/reconciliation for post-enqueue exceptions (not timeout guesses).
- Add device/queue/tenant generation attestation and bounded event/operation retirement under kernel failure and device reset.
- Run this native actor + DOP kernel probe on actual GPU-equipped authorized CI, with driver/model identity, kernel SHA, source SHA, event and independent oracle evidence.
- Connect Java and LLVM compiled GPU-actor MIR/effects and sealed state to this provider, before claiming AttoLang GPU actor execution.
- Test supervisor tree crash/restart beyond the in-process root; then backends .net/.wasm/.forge with explicit fail-closed unsupported-device gates.
