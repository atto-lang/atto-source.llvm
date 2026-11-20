# GPU actor V0 direct test seam removal

This draft removes the `ORESLANG_GPU_ACTOR_V0_HOST_TEST_SEAM` conditional compilation path entirely. Completion, cancellation acknowledgment, native buffer release and direct V0 turn mutations remain private to `HostTurnBridgeV0`, exposed only to the trusted V1 supervisor-registry friend.

`tests/gpu_actor_host_turn_bridge_v0_test.cpp` is rewritten to exercise 64 actors and four turn sequences per actor **through V1 registry-mediated provider events**; cancellation, generation rejection, stage overflow and supervisor CONTROL retirement remain covered. No test directly calls a V0 Boolean completion function.

**Coverage change:** 6,257 old direct-V0 assertions were replaced by 1,169 registry-migration assertions. The existing separate 6,329-assertion V1 suite remains unchanged. This is a safer coverage migration, not a claim of identical assertion coverage.

The exact source files, as stacked with the recovery-worker V3 branch, passed GCC ASan/UBSan and Clang private CI: https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42958180311 . Host-only actual repo CMake/CTest passed five selected targets per compiler. These tests do not prove physical GPU completion or compiled AttoLang guest execution.

**Security limit:** C++ private methods are compile-time encapsulation, not a cryptographic or runtime isolation boundary against malicious linked native code. Native providers remain privileged and untrusted guest FFI must not load them.
