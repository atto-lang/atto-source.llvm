# PR #273 — root exceptional-control semantic reconciliation

Historical head `e97744eb49ceab8da4734ec8d00e84c71451dc1a`. Latest LLVM main `e5c9973d97199e26f13157dbffbaffc192bb9993`.

Preserve current main's `EffectUnwind`, `EffectSupervisorBridge`, `SupervisorControlTurn` and `SupervisorFailureLane` implementations, including source/host isolation and independently evolved terminal-cancellation semantics. Do not replace them with stale copies from the historical PR.

Forward-port only the host-only `root_terminal_outcome` policy and its test, registered as `native_nonactor_root_failure_boundary`. The original root-policy header lacked modern `EffectKind::ProcessExit` and mapped it to a fatal error; this integration treats explicit ProcessExit as a separate terminal control signal, preserving its supplied exit code for the caller. An invalid discriminator fails closed through main's canonical `normalize_effect` (Fatal 0xFFFFFFFD), retaining original diagnostic origin. Ordinary unhandled Throw/Raise normalize to Panic only at the root, not at intermediate callers.

No source-language `throw/raise/panic/recover` lowering, actor-supervisor integration or Java/LLVM parity is certified. The original larger semantics contract remains historical context, not a permission to overwrite current policy. Run exact-head CMake/CTest (with -Werror), ASan/UBSan and compiled guest tests before merge.
