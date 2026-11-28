# Bounded AOT layout traces for inverted classes (native host substrate)

**Status:** native C++ host component. Not compiled `.atto` source, guest actor-state integration, source `define inverted class`, a JIT compiler or evidence of whole-process zero-GC. No new code is generated at runtime.

`DopAotLayoutTrace.hpp` is a **64-field fixed-capacity** trace buffer for a caller-owned data store. It records valid row/field observations without heap allocation, locks, callbacks, wall clocks, I/O or executable-memory writes. It defaults to **off**; sealed/hard-real-time policies freeze enablement at construction. Each hot-path probe rejects invalid coordinates, counts saturate rather than wrap, and the buffer never returns native pointers or acts as an actor authority. Only one trusted actor turn may mutate an instance; sharing it between concurrent threads is unsupported.

The observation order provides two optional **heuristic** hints:

- Same-row transitions across different fields: AoS candidate.
- Same-field scans over consecutive rows: SoA candidate.

At least two transitions and a strict winner are required. Unknown, tied, short and random workloads produce `Insufficient`. Neither choice claims measured speedup, automatic AoSoA/SoAoS tuning, a layout migration, SIMD vectorization, or optimal cache placement. This is specifically useful for sampling the access shapes associated with *inverted-class/columnar storage*, without disabling sealed AOT or requiring a tracing garbage collector.

The compiler could later insert already-compiled, opt-in observation sites in its **trusted instrumentation profile**. A future governed ORC JIT is a separate *non-sealed, non-hard-RT* execution profile, constrained to finite compiler-known type identities and verified typed MIR. No host-only trace observation may request source evaluation, hot-swapped code, unbounded specialization or layout mutation. In sealed full guest AOT the only valid follow-up is an offline profile-guided **rebuild** (or selection among precompiled kernels) after ownership and generation/loan fences. In-place relayout requires the independent `DopCheckedRegion` handle/epoch safety checks and complete actor ownership rules.

## Tests and admission

`tests/dop_aot_layout_trace_test.cpp` checks disabled/invalid probes, SoA/AoS/insufficient hints, reset, sealed policy, field bounds, 64-bit row edges and transition history. CMake registers `native_dop_aot_layout_trace_host_only` as a strict C++17 test (no Python). Run with both GCC/Clang, O0/O2 and ASan/UBSan when available. This is *native host evidence* only; it is not compiled `.atto` guest output or Java/LLVM differential parity.

Next: exact-head compiler-generated probes, actor/owner/generation fencing, source-compiled inverted-class guest tests on Java and LLVM, offline reproducible PGO benchmarks (p50/p95/p99, RSS, binary size), and source-local or separately authorized stepful CI. Never count `steps=[]` as execution. Retain the zero-GC memory-reclamation requirement for long-lived actors independently of this telemetry component.

Rust-only matrix orchestrator (no Python, no external Rust crates):

```sh
rustc --edition 2021 tools/test-dop-aot-layout.rs -o /tmp/test-dop-aot-layout
/tmp/test-dop-aot-layout .
```

The orchestrator builds and **executes** the native C++ test under GCC/Clang, O0/O2 and ASan/UBSan. Toolchain availability, compilation and execution are required; missing tools do not silently skip validation.
