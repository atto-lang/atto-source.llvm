# DOP CPU/GPU execution readiness — 2026-10-15

**Release status: NOT READY for general source-level heterogeneous execution.**
This document is an implementation gate, not a declaration that GPU kernels or
ordinary Oreslang `cpu {}` / `gpu {}` blocks are executable.

## Current audited foundations

- Native `main`: checked homogeneous POD AoS/SoA/AoSoA geometry, typed f32
  scalar CPU execution, OpenCL C text generation, a host buffer fence ledger,
  and selected CTest contracts. These are not source-language DOP lowering,
  SIMD codegen, a GPU device backend, or an actor compute graph.
- Java/Graal: domain keywords are reserved/fail-closed on `main` (PR #700);
  typed layout, CPU kernel, GPU preflight and region/ECS/mapper experiments are
  staged across PRs #566/#579/#703/#215/#222, not a single merged callable.
- LLVM #83 and Java #593 are **draft**, restricted accelerator/source graph
  profiles, not general production `Region<T>` or portable GPU execution.
  OpenCL **CPU** device execution is not evidence of a physical GPU pass.
- CPU batch isolation: require whole-destination exclusive write loans (native
  PR #445, Java PR #703), not per-element mutex locking. Destination admission
  must fail before the first store if a borrow, transfer or close conflicts.

## Execution contract to implement, in dependency order

1. **Canonical logical-region ABI.** Define `Region<T>` with a bounded
   homogeneous scalar/struct subset, stable field IDs, checked shapes, explicit
   immutable views, and physical `AoS|SoA|AoSoA<N>` layouts. Separate semantic
   field identity from physical layout. Establish layout and endianness vectors
   shared by Java and LLVM; reject pointer-containing fields and unknown
   padding/stride/alignment. Overflow and partial initialization fail closed.
2. **Typed region effects and dependency DAG.** Every compute callable declares
   field-level `reads`, `writes`, `discards`, or explicit `reduces(op)`.
   Static ownership/alias proofs plus runtime lease enforcement are required.
   Overlapping writers serialize, disjoint field partitions may parallelize,
   and invalid reductions/hidden I/O/dynamic allocation reject. A read-only
   view never grants a mutable or sendable alias.
3. **CPU backend first.** General source program -> typed DOP IR -> executable
   scalar LLVM + Java/Graal reference -> controlled SIMD/vector variant ->
   multicore scheduling. Check CPU feature detection, legal vector width,
   aligned/unaligned tails, NUMA partitioning, cancellation safepoints, actor
   scheduling fairness, and explicit deterministic reduction mode. No shared
   mutable region escape between actors. Prove zero tracing-GC in the native
   hot path; measure allocations on Graal rather than assuming zero GC.
4. **GPU capability and execution.** Source GPU body must pass restricted effect,
   shape, precision and bounded-launch verification. Model typed
   `HostBuffer<T>`, `DeviceBuffer<T>`, generation/tenant/device identity,
   explicit upload/download and async dispatch. Atomically claim both device
   fences and quotas; require provider-authenticated provenance and physical
   completion before releasing memory/credits. Reject missing/unsupported GPU,
   device loss, cancellation races and tenant/device aliasing; NEVER implicitly
   execute a `gpu` body on the host. A cost-based AUTO mapper can choose CPU
   only where the source explicitly permits both placement variants.
5. **Actor continuation and ownership.** Suspend the calling actor without
   parking a scheduler carrier; resume once after a verified device event on
   its owner DATA/CONTROL path. Bound queue depth, pinned bytes and in-flight
   allocations. No raw device/host pointers in Sendable messages or cross-isolate
   transport. Close/shutdown is fenced against device commands and stale events.
6. **Production verification.** Compile identical positive/negative `.ores`
   fixtures in Java and LLVM. Run CPU reference/SIMD parity and physical-GPU
   upload -> launch -> download parity for every layout, including non-multiple
   tile lengths, empty CPU data, NaN/Inf policies, overflow, misalignment and
   mutation conflict. Test ASan/UBSan/TSan, actor cancellation, forced OOM,
   device reset, tenant budget and ABA generations. Record device vendor,
   driver, compiler flags, SHA/tree and test-step evidence. An OpenCL CPU ICD
   is a useful *secondary* check, not the hardware-GPU release gate.

## Numerical modes

- Default reproducible mode: define float contraction, reassociation, signed
  zero, NaNs/subnormals and reduction tree semantics across backends; reject a
  device that cannot implement the declared contract. Do not claim bitwise
  equivalence merely because OpenCL source includes `FP_CONTRACT OFF`.
- Opt-in fast mode: explicitly permit target-dependent floating error, record
  absolute/relative tolerance and ULP budgets, and never use this mode to
  bypass ownership, memory safety or effect restrictions.

## Benchmarks required before enabling automatic placement

Compare AoS/SoA/AoSoA on small, medium and large datasets (including non-tile
sizes) with CPU scalar, SIMD, multicore, OpenCL CPU ICD and physical GPU.
Measure effective GB/s, ns/element, speedup, allocations, p50/p95/p99 latency,
energy where available, host/device transfer cost, queue wait, and memory
footprint. Benchmark cold and warmed Graal JIT/AOT separately. Derive GPU
crossover thresholds from measured end-to-end transfer+launch costs, not
kernel-only time; use a C++/Rust reference with identical arithmetic/layout.
Publish a reproducible manifest per supported target.

## Do not merge/enable as a single broad switch

Merge checked geometry and CPU isolation independently. Next land source-level
CPU regions/effects + owner leases; then SIMD and parallel executor proofs;
then limited device-safe GPU IR and genuine provider execution; finally typed
nested CPU/GPU scopes, actor continuations, and AUTO mapping. Unknown target,
missing backend or incomplete proof always rejects with a stable diagnostic.
