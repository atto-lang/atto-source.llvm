# Future work and migration roadmap

Status: proposed future work, 2026-10-08. All implementation milestones below are open.

## Current boundary

Keep development and releases in oreslang-source.java and oreslang-source.llvm independent. This repository starts with documentation only. Do not copy entire backend implementations here or migrate consumers merely to populate the repository.

The immediate shared work is the language specification plus TypeSpec/JSON Schema conformance contracts and identical source fixtures. Keep their existing backend integrations working; centralizing their distribution later must not stall either compiler. Their presence does not mean a shared semantic compiler exists.

## Phases and exit criteria

| Phase | Work | Exit criteria |
| --- | --- | --- |
| 0. Independent progress and shared contracts | Continue both compilers/borrow checkers; expand common specifications, schemas, fixtures and parity tracking | Explicit backend readiness and executed semantic evidence; pending is not pass |
| 1. Specify a minimal IR | Define typed HIR and MIR for literals, arithmetic, local bindings, branches, functions and calls | Versioned examples; malformed-input and verifier tests; defined numeric behavior |
| 2. Implement one vertical slice | Lower the subset from HIR to MIR and build one adapter for each backend | Same fixtures execute on real LLVM and Java/Graal paths with matching specified results and failures |
| 3. Introduce shared consumption | Package the common layer and pin versions in each backend independently | Reproducible builds; compatibility checks; fallback and rollback paths |
| 4. Expand language semantics | Add aggregates, generics, ownership/borrowing, cleanup, errors and modules in bounded increments | Positive and negative coverage at HIR, MIR and backend execution levels for each feature |
| 5. Add runtime-sensitive features | Specify and lower async, actors/channels, suspension, capabilities and isolation | Real runtime tests for cancellation, cleanup, progress and isolation; unsupported combinations fail explicitly |
| 6. Prove trusted AOT and consolidate duplication | Prove complete trusted LLVM AOT; pursue generated-classfile Graal guest AOT; move only frontend/semantic code demonstrated equivalent | Trusted actors and async work without JIT/interpreter fallback on LLVM; Graal status reported independently; removal has a rollback plan |
| 7. Optimize and stabilize | Evaluate shared passes, target-specific legalization, incremental compilation and release policy | Semantic regression gates plus reproducible performance measurements on supported targets |

No calendar dates or performance targets are committed yet. Phases may overlap for experiments, but consumers should not bypass their acceptance gates.

## Parallel backend priorities

These are independent workstreams, not prerequisites for beginning useful work in the other backend:

- **LLVM:** expand control flow/types, implement its own borrow checker, add aggregates/generics and native actor heaps, lower async/channels/preemption to continuations, and prove full trusted-code AOT. Later add authorized untrusted-code ORC JIT and a separately verified strict realtime profile.
- **Java/Graal:** continue semantic/runtime work; prototype MIR → JVM classfiles → Native Image for actual program-code AOT. Keep the Truffle path for interactive execution and optional JIT.
- **Shared contracts:** prioritize ownership lifetime violations, suspended borrows, single-writer actors, cancellation, automatic preemption and generation-safe hot loading. Add a Graal result adapter so ordinary programs can be compared by actual values, exceptions and effects.

Automatic preemption must cover compiler-generated and collection loops as well as ordinary source loops. Effects, purity, allocation/resource contracts, cyclic references, macros, SIMD, FFI and tail-call guarantees belong in the feature ledger with explicit implementation and test status. Native hardware guarantees remain capability-scoped.

### Cross-backend memory and performance release gates

Follow [the zero-GC native / low-GC Graal contract](MEMORY_SAFETY_AND_PERFORMANCE.md): LLVM must have collector-free transitive native execution and deterministic source-level reclamation; Java/Graal must measure and minimize allocations in warm hot paths while retaining truthful host GC reporting. Neither is yet fully proven. Add a shared deterministic game-tick/ECS workload plus C++20/Rust baselines, exact-source verification, and positive/negative ownership, cancellation, actor/FFI retirement and cycle tests as explicit release gates. Do not confuse an allocation-free frame with a no-GC process, or an arena microtest with full borrow safety.

## First implementation tasks

- [ ] Write an initial architecture decision for the shared implementation language and package boundary.
- [ ] Choose and document a minimal source-language subset.
- [ ] Resolve integer width, overflow, division/error and evaluation-order semantics for that subset.
- [ ] Define HIR/MIR examples with source locations and stable semantic identities.
- [ ] Choose the MIR control-flow/value model and write its verifier.
- [ ] Implement a small deterministic HIR → MIR lowering pass.
- [ ] Prototype the LLVM adapter.
- [ ] Prototype and choose the Java/Graal integration path.
- [ ] Execute a common source corpus through both real adapters.
- [ ] Add negative controls for compiler crashes, timeouts, missing tools and unsupported operations.
- [ ] Document exact commands, source revisions, toolchain versions and executed results.

## Questions requiring a design decision

1. Which frontend functions should eventually move here, and which remain backend-specific?
2. How is the common layer implemented and consumed across native and JVM environments?
3. Is MIR SSA-based, and how are exceptional exits, cleanup and suspension represented?
4. Where do generic specialization and target-dependent layout selection occur?
5. Which runtime capabilities are mandatory for each execution profile?
6. How will diagnostics and source mappings survive optimization?
7. What is the version/compatibility policy for IR, adapters, caches and runtime operations?
8. Which Java/Graal integration path supports the required JIT and AOT behavior?
9. What measurable evidence justifies retiring a duplicated semantic implementation?

## Migration safeguards

- Introduce the common path as an explicit experimental option first.
- Pin every consumer to an immutable, tested revision or release.
- Keep the independent path usable until its replacement is verified.
- Compare actual observable behavior, not only generated IR or parse acceptance.
- Require executed tests for the exact candidate artifacts; stale or zero-step CI is not approval.
- Preserve backend-specific regression suites and architecture constraints.
- Avoid synchronized mandatory releases until compatibility testing supports them.
- Move code in reviewable slices; do not combine runtime rewrites with the first common-IR adoption.

## Design provenance

This plan incorporates the user-provided transcript of the architecture discussion:

https://chatgpt.com/share/6ac8206c-6088-83e9-9b42-7fae384fcf11

The later user corrections govern the plan: independent compilers and borrow checkers now; complete trusted-code AOT required eventually in LLVM and targeted in Graal; untrusted guests may use JIT and do not require AOT; shared source consolidation comes later.

Historical test results in that discussion identify useful starting points, not validation of future revisions. See [conformance](CONFORMANCE.md) for how to record evidence.
