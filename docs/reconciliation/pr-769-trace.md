# PR #769: scoped native failure-trace forward-port

Historical branch `073e597c824aa979473c88b7d8cb945f62bef61d`; current LLVM main `6ea1af47b97fe24a1d5e09e8feba1a8d67b03a5a`.

Port only `NativeFailureTrace.h/.c` and host-only `native_failure_trace_test.c`, wired to the **current** verified execution-context/continuation/await/logical trace stores. Those store dependencies are already in main and `NativeLogicalTrace.h` matches the historical blob exactly. The runtime captures logical ancestry while holding a temporary generation-checked execution handle retain/release and renders bounded text without heap allocations. Reject invalid THROW/RAISE/PANIC discriminators and stale execution handles; preserve current store lifetime and Task/borrow semantics, not historical source emitter snapshots.

This is a **host recorder** and does not yet hook the actual source-language `throw/raise/panic` exceptional edges; native stack symbolization, actor-message and supervisor-control causal annotations remain separate tasks. The old PR was stacked on #768; its historical source compiler changes are intentionally deferred to avoid undoing current source actor lowering and ABI contracts. Run exact-head CMake/CTest under ASan+UBSan and compiled guest traces before declaring parity.
