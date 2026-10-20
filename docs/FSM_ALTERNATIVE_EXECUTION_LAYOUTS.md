# RFC: Oreslang FSM alternatives and LLVM data-oriented execution contract

**Status:** DRAFT / design plus host-only differential test, 2026-10-19. No new `.ores` syntax, native FSM lowering, ECS scheduler, AOT performance, or cross-backend equivalence is implemented or claimed here.

## Scope and relationship to concurrent work

This is the **LLVM/native companion** to the userland reducer/ECS design in [oreslang-design-patterns#7](https://github.com/ores-truffle-oreslang/oreslang-design-patterns/pull/7), the symbolic Statechart/Petri semantic proposal in [Java#1522](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1522), and its physical-layout/ECS host contract in [Java#1638](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1638). This RFC specifies alternative execution paths and the minimum native observables needed for genuine guest trace parity. It is deliberately **not** a competing canonical frontend API, replacement layout planner, or assertion those draft PRs have merged. Reconcile their contracts before implementation.

**Decision to review:** prefer one pure, deterministic, typed transition *semantic kernel* with multiple library-level machine families and one selected physical layout per active store. Treat ECS, actors and task pipelines as **optional scheduling/composition adapters**, never synonymously as FSMs or memory layouts. Do not add a bespoke `fsm` keyword in v0.

## Alternative machine families

| Family | Event/transition execution | Strength | Cost / limitation | First prototype |
| --- | --- | --- | --- | --- |
| A1. Pure reducer (`step(state, event) -> Result<Next, Rejected>`) | Closed typed `match`/`switch`, no I/O | Predictable, embedded, actor-free, replayable; easy exhaustive tests | Dispatch branch cost for many sparse rules | **First** |
| A2. Static sparse transition table, generated from A1 schema | Compile-time normalized `(state,event)->guard,action,next` map | Compact protocol/workflow definitions, deterministic AOT | Explicit rule priority and missing-transition policy needed | **First**, check equivalence to A1 |
| B. Hierarchical statechart / state machine | Leaf-before-ancestor fallback; explicit finite event queue | Nested lifecycles and OTP `gen_statem`-like policies | Orthogonal regions/microsteps can explode; require cycle/fuel bounds | **After flat model**, per Java#1522 |
| C. Place/transition Petri net | Enabled transition atomically consumes/produces bounded token markings | Resource workflows, independent-event commutation, conservation proofs | Capacity, fairness and concurrent commit complexity | **After flat model**, per Java#1522 |
| D. Reactive behavior tree | Priority/selector/tick semantics, with FSM as leaf adapter | Agent/planner behavior | Tick ordering != FSM event semantics; not a substitute for a reducer | Optional userland |
| E. Actor-driven `gen_statem` adapter | Mailbox events dispatch into A/B/C kernel | Supervision, backpressure and fault containment | Per-message overhead; should not spawn actor per entity/state | Optional `actor`/`std/otp` |

A1 and A2 must produce the **same** observable `(accepted/rejected, next_state, version, ordered_effect_ids)` trace. B/C need explicit *separate* semantic adapters: do not equate Statechart microsteps with Petri firings. Each family can execute without actors, ECS, dynamic allocation, or threads when capacities are fixed.

### Illustrative API only — NOT validated guest syntax

```text
// Pseudocode: exact Oreslang generic/result/symbol syntax remains an open review.
const next = step(machine, #start); // typed outcome, no I/O inside step
commit(machine, next);               // one admission/commit boundary
emit(next.effects);                  // emit only after successful commit
```

Use canonical symbolic IDs in proposed schemas where supported, not raw pointer identity, array offsets or undocumented hash seeds. Existing `#symbol` process identities do not grant cross-trust-domain authority. Export/serialization requires explicitly authorized stable, versioned mappings; unknown keys must reject before mutation. No dynamic runtime `Symbol` discovery may silently add AOT reflection roots.

## Semantics to preserve independent of physical layout

1. Each machine admits an event against a single immutable input version. Unknown edge, failed guard, exhausted bounded effect outbox, invalid domain or arithmetic overflow returns **Rejected** with *identical* state/version and zero effects. A successful self-edge is distinguishable from rejection and increments the version once.
2. Guards are pure and evaluated against the pre-state. Transition effects are **descriptions** of external work, staged in a bounded ordered outbox and released after successful commit. A failed later guard or capacity reservation must not leak prior effects.
3. For batch ECS ticks, define stable entity ordering and event ordering, regardless of archetype movement, physical row order, SIMD lanes or worker/carrier assignment. If ordering intentionally changes, encode that policy and compare explicitly (not silently).
4. Actor integration still has exactly **one `state { ... }`** for persistent guest actor state and uses the checked critical `handle_state` / state-loan boundary; no free-standing actor instance fields or mutations from a parallel worker. The kernel itself is actor-free.
5. Cancellation, retries and hot reload must use `machine_schema_version`, commit/version gates, bounded retries, and explicit upgrade/rollback. A cancelled transition that has not committed emits no effects; post-commit external delivery must use sequence IDs/idempotency and separate replay semantics.

## Physical layout and ECS are orthogonal decisions

| Storage representation | Meaning | Typical win | Hazards to prove |
| --- | --- | --- | --- |
| **AoS** | Contiguous records `(state, version, payload)` | Sparse random per-entity complete updates; small machine count | Scans drag unused fields into cache |
| **SoA** | Columns `states[]`, `versions[]`, `payloads[]` | Homogeneous column filtering, vectorized guards and counters | Cross-field scatter, alias analysis, partial-write rollback |
| **AoSoA** | Tiles of lanes, columns inside each tile | SIMD/coalesced per-tile updates, local field grouping | Padding, masked final lanes, tile stride/alignment, lane ownership |
| **ECS archetype/chunk** | Group entities by component signature; query matching cohorts and schedule systems | Heterogeneous compositions and batching | Generational IDs, structural barriers, chunk relocation, declared read/write sets; **not a fourth layout** |

An ECS archetype can internally choose **one** of AoS / SoA / AoSoA for the FSM component payload, and different archetypes may use different layouts. A singleton state machine should need none of these container abstractions. The native [`DopLayout.hpp`](../src/main/cpp/oreslang/runtime/DopLayout.hpp) already computes bounds-checked POD AoS/SoA/AoSoA byte geometry; this RFC reuses it, not another offset formula. Existing native `DOP_MULTI_LAYOUT_FORMAL.md` investigates *multi-residency with versioned coherence* separately; **do not** accidentally maintain three writable replicas with no authority or refresh protocol.

**Conversion and ownership contract:** before any AoS↔SoA↔AoSoA migration, acquire the exclusive region/loan barrier, preflight target byte size, alignment, quota and arena provenance, reserve bounded scratch, transpose to new storage, atomically install it, invalidate pinned row/column views by epoch, and only then release old storage. Stable ECS IDs carry owner/region + slot + generation and must fail closed after slot reuse (ABA). Archetype swap-delete must update the ID-to-physical-row mapping; a stale physical row index is never an authoritative handle. Conversions are observable synchronization points but should not modify logical machine state or version.

**Scheduling:** systems declare read/write sets of fields and **proven** physical regions. Disjoint proven region loans or non-conflicting access sets may run in parallel; an unproven/unknown provenance is conservatively rejected, not treated as disjoint just because strings or owner integers differ. Structural ECS add/remove/move operations are deferred to a barrier; deterministic batch commit sorts by logical entity ID where required. A runtime bitmask alone is not a compiler provenance proof.

**Memory/RT profile:** AoSoA lane/tile widths should be compile-time bounded and backend-specific; require checked `rows*fields*width`, tile count overflow, alias-free field offsets, and final-tile masks. Native zero tracing-GC can use caller-owned arenas with deterministic drops, but dynamic layout migration, effect allocations, unbounded Petri reachability or OS scheduling do not automatically qualify for freestanding / hard real-time WCET. No hidden `malloc`, RTTI, clocks or reflection in a true RT profile.

## Native host reference gate in this PR

`tests/fsm_layout_alternatives_test.cpp` compiles the **real** native POD offset planner and exercises an A1 flat reducer across all three layouts, five entity slots and every event word of length 0..4 (1,023 layout/trace combinations; all applied to all five entities). It additionally checks AoSoA partial tiles, logical snapshots through migration, exclusive borrow/epoch invalidation, wrong-owner/slot/generation rejection, an explicit stale-handle reuse test and conservative ECS read/write admission examples. A2 table generation and B/C adapters are **design candidates**, not implemented in this small fixture.

Run standalone without LLVM installation:

```sh
bash scripts/verify-fsm-layout-alternatives.sh
```

The fixture is a finite **host-side reference**: it does NOT test actual Oreslang symbols, compiled `.ores` reducers, actor-state loans, provenance proofs, LLVM IR emission, AOT determinism, hot-reload atomicity, scalar effect outboxes, cross-backend parity or performance. The hardened host model **does not accept** a caller-supplied `proven_region` flag. Stores now mint unique, non-null allocation-identity tickets for ECS access plans and handles, and each borrow has a separate loan ticket; foreign stores, replayed/released views, and empty/fabricated loan tickets reject. Copy/move of a live Store is disabled to prevent accidental identity duplication. These C++ reference tickets are not a guest ABI, an actual compiler-issued proof, or cryptographically unforgeable user capabilities. Real guest provenance must be minted/verified by Oreslang's ownership and region admission logic.

## Independent exact-source host validation (2026-10-20 UTC)

- **Code commit:** `e3374602090bf6cfd28c46dadaa8f5c44536f373` on this LLVM draft PR, after cross-store/loan-replay/forged-access hardening.
- **Private test destination:** `hacker-house-medellin-test/api-contract-e2e`, temporary `hardening/fsm-llvm-20261019`, commit `433dd5d1c4038f961d4268d89a217929002e895c`.
- **Exact source blobs:** FSM C++ test `e3fa48de6c199cce8f9223e6d233df77993477fc`; repository production `DopLayout.hpp` `fea958c2fffd2a3023819c7afff2a8ddc9e82db3`; test script `b49d917d115f5007f1d91ea4bb83b73715dee871`. All three identities matched destination Git blobs and runner `git hash-object` checks.
- **Successful [GitHub Actions run #39367374914](https://github.com/hacker-house-medellin-test/api-contract-e2e/actions/runs/39367374914):** both jobs completed, strict g++ C++17 compilation and clang++ C++17 + AddressSanitizer/UndefinedBehaviorSanitizer passed, each executed all 1,023 finite layout/trace combinations and adversarial cases.
- **Security:** no cross-organization checkout token, newly introduced secret, or public mirror; pinned read-only checkout and exact-blob verification. Only the three necessary private source artifacts were transferred (plus destination-only CI workflow).
- **Limits:** this is not the full LLVM repository tree and cannot prove guest compiler execution, region capabilities, real parallel ECS, crash consistency, WCET or AOT. Do not claim production readiness.

## Implementation / merge gates

- [ ] Reconcile A1/A2 exact Symbol ID, result/guard/action semantics with design-patterns#7 and Java#1522; publish a shared language-neutral canonical trace schema.
- [ ] Implement and test real `.ores` reducer/table positive and negative admission fixtures on current Java and LLVM compiler heads. Reject unsupported constructs instead of silently using host fallbacks.
- [ ] Prototype userland Statechart/Petri adapters as distinct trace contracts; explicitly bound microsteps/tokens and inspect fairness, no-deadlock assumptions and event conflict policy.
- [ ] Implement authenticated native region + generation IDs, compiler-checked field/partition loans, real ECS structural barriers, and rollback-safe migration. Compare against Java#1638 at real guest level.
- [ ] Add fixed-seed and adversarial differential traces (states, rejection reason, effects, versions, cancellation order), exact source/blob and binary identities, sanitizers, and negative mutation checks on **both** backends.
- [ ] Benchmark representative workload density (1, 16, 1,024, 1M entities), event skew, component cardinality, tile widths (2/4/8/16), cache misses, bytes/instance, SIMD throughput, mean/p95/p99, transposition cost and worst-case admission; report hardware/compiler flags. Do not assume SoA or AoSoA universally wins.
- [x] Execute the current native host fixture against the **exact three source blobs** in a private independent Actions capsule using strict g++ and clang++ ASan/UBSan checks. This is a source-blob-verified capsule, **not** a full-repository Git-tree mirror.
- [ ] Record guest-backend end-to-end CI evidence on a complete exact source PR tree. A zero-step Actions startup failure is **no test evidence**. Any private full-tree mirror must be explicitly authorized private→private and prove identical root Git-tree SHA before execution. A scoped testing capsule must separately verify all transferred source Git blob SHAs and never claim full-tree identity.

**Do not merge as a production FSM / ECS feature.** This PR is a reviewable design + host-model gate for the native backend, intentionally complementary to existing draft designs.
