# Native AoS / SoA / AoSoA multi-residency reference

This is a limited executable native header-level refinement test for Java PR #1477 and the seven-subset abstract model in formal/tri_layout_model.py of that PR.

tests/dop_multi_layout_formal_test.cpp compiles the real existing zero-allocation C++ DopLayout.hpp address planner, then creates one, two, or three independent bounded physical byte buffers over three rows, two POD 32-bit fields and tile width two. It crosschecks each physical layout with an independent logical row oracle, over seven nonempty residency sets, all selected resident write authorities, and all pairs of selected rows. It requires exactly 216 schedules with safe refresh, version authority, 16-byte aligned AoSoA partial-tile geometry, and a stale-cache write mutation that MUST be detected.

The test is a C++ host-side model with explicit std::vector-backed buffers. It neither installs a native compiler backend nor proves LLVM-generated guest code, per-actor arenas, zero tracing-GC runtime behavior, full AOT, ECS scheduler permits, concurrent mutation or group-owned ActorRef semantics. The DopLayout.hpp pure offset planner is the only current real native production component directly exercised; the mirror/coherence manager is a reference test harness, not yet runtime implementation.

Before promoting real ECS and layout-selectable native regions:
- Compile real Oreslang guest source for both LLVM and Java with 1/2/3 resident layouts and compare exact logical values and errors.
- Prove authority generation/epoch fencing, runtime arena lifetime, pinned row/field borrows, atomic strong-read reconcile and cancellation rollback.
- Check per-tenant budgets, scratch transpose space, overflow, alignment, schema/ABI and AoSoA tail padding for widths beyond two.
- Run actual all-steps-executed exact-head CI with native sanitizers, not a zero-step billing/startup failure.


## Added verification hardening

A second mutation forges a fresh SoA version without transposing its stale payload; a third mutates physical authoritative bytes without publishing a logical commit. Both must be rejected by independent content checking. A malformed AoSoA second-tile stride creates overlapping offsets and is compared with the actual native planner. Hydrating absent residency must throw. The mutation checks are part of the same exact-source compiled C++ executable; they are **reference-level** checks rather than proof of atomic runtime implementation or concurrent linearizability.
