# GPU actor V0 completion authority audit — 2026-11-10

**Scope:** LLVM native host-turn ledger, provider V1/V2 stack. **Status:** security hardening verified on exact native source, **not** physical GPU execution, compiled AttoLang guest support, or a release-ready device trust boundary.

## Finding: public raw Boolean completion authority

`HostTurnBridgeV0` previously exposed methods accepting Boolean claims (`successful=true`, `provider_event_quiescent=true`, `provider_released=true`) to any ordinary native translation unit that included the header. That permitted a native caller to bypass the V1 provider event identity and quarantine checks and publish a staged actor value or retire a device buffer without device-event evidence.

The affected methods include `finish_after_provider_event`, `acknowledge_cancelled_provider_event`, `acknowledge_backend_buffer_release`, and `retire_gracefully_after_backend_release`; direct turn preparation/enqueue sequencing and `abort_enqueue` are also private in ordinary builds.

## Fix

These proof-sensitive methods are **private by default**. Only the friend `GpuActorQuarantineRegistryV1` (including its trusted nested Entry) may invoke them in the ordinary native runtime build. `request_close`, `report_device_loss`, and read-only diagnostics remain callable.

`tests/gpu_actor_host_turn_bridge_v0_test.cpp` defines `ORESLANG_GPU_ACTOR_V0_HOST_TEST_SEAM` before including the header. This intentionally preserves the existing V0 host-ledger regression suite, while giving the exceptional test-only bypass an explicit source marker. **Do not define this macro in runtime or guest-facing builds.** This is compile-time API hardening, not a cryptographic proof of device authenticity and not a sandbox against malicious native C++ code. Trusted native code and alternative `ProviderPortV1` implementations still require independent review.

`tests/gpu_actor_v0_completion_authority_negative.cpp` attempts four unauthorized Boolean completion/release calls. GCC and Clang must reject it *for private-access reasons*, not unrelated compilation errors. `tests/cmake/GpuActorV0CompletionAuthorityGate.cmake` checks that predicate with the compiler passed by CMake, and `CMakeLists.txt` registers the gate as `native_gpu_actor_v0_completion_authority_private` when BUILD_TESTING is enabled.

## Executed exact-source proof

[Private GHA carrier #42953785708](https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42953785708) in `scintilla-run/scintilla-run-e2e` (branch `carrier/gpu-opencl-v0-authority-20261110`):

- The private source files were mirrored by the connected GitHub API into a temporary carrier-local tree. **Git blob SHA identities checked before compilation**; no cross-organization checkout, personal access token, or source-org Actions minutes.
- GCC C++17 strict flags, AddressSanitizer/UndefinedBehaviorSanitizer: 128 V2 API adapter assertions; 6,329 V1 actor/quarantine assertions; 6,257 V0 host bridge assertions — all passed.
- Independent Clang C++17 strict flags: the same 128, 6,329 and 6,257 assertions passed.
- Preexisting OpenCL G3 provider state-machine native suite: passed with GCC ASan/UBSan.
- Direct GCC + Clang compile-negative tests confirmed all four direct V0 authority calls are inaccessible in ordinary builds; the standalone persistent CMake gate script independently passed for both configured compilers.
- The physical native probe was compiled and attempted, but reported `GPU_ACTOR_V2_NO_GPU_RUNTIME`; physical GPU execution is **NOT_EXECUTED**, and no CPU fallback was attempted.

The large CMake project was **not configured or run as a whole**. The reusable CTest script was executed directly in the carrier; full repository configure/build/CTest remains a separate acceptance gate.

## Remaining production blockers

1. **Native provider trust still exists:** `ProviderPortV1` is a privileged C++ interface; a malicious or buggy trusted native provider can mint a local event and falsely claim device quiescence. `HostTurnBridgeV0` private access is not a runtime security boundary against hostile native binaries.
2. **Test macro must be excluded from production builds.** Remove the legacy test-only seam entirely when V0 tests are migrated to the registry-backed capability tests.
3. The ambiguous-upload/kernel/download recovery methods may block on `clFinish`. Integrate a supervised recovery worker with bounded resources and explicit crash/restart handling. In-process retention is not crash durable.
4. Obtain real GPU hardware event and kernel output proof with driver/device metadata; no compatible GPU was available for this run.
5. Complete executable guest GPU actor HIR/MIR and real kernel lowering, then test the five backends with independent evidence or fail-closed unsupported-device gates.

**No PR was merged; this change must remain draft pending review and physical/guest integration.**
