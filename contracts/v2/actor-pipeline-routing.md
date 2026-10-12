# Actor pipelines: bidirectional routing and lifecycle contract (draft v1)

Status: **bounded three-actor guest execution verified on LLVM and Java/GraalVM JVM; general routing and cross-backend lifecycle refinement still in design**.
Java/GraalVM and LLVM must give these operators identical observable semantics
before all of `self.forward`, `self.backward`, `self.respond`, and `self.done`
are enabled as general guest intrinsics. A stub, dynamically looked-up actor method,
ordinary `send` alias, or immediate success without delivery is forbidden.

## Operations and recipients

| Operation | Destination | Required authority |
| --- | --- | --- |
| `self.send(target, message)` | Explicit actor ref | Compatible destination contract |
| `self.forward(message)` | Selected outgoing pipeline edge | Runtime-minted downstream route |
| `self.backward(message)` | Selected incoming/reverse edge | Runtime-minted upstream route |
| `self.respond(value)` | Correlated request origin, **not** preceding stage | Live typed reply capability |
| `self.done()` | No implicit destination | Current delivery's stage-completion right |
| `self.fail(error)` | Pipeline failure coordinator and eligible waiter | Live failure right |

A linear actor pipeline may call `forward` and `backward` without explicit
recipients. At a fan-out/fan-in node, an unspecified ambiguous edge **must
fail** (compile-time if topology is static; runtime admission if dynamic).
Future port- or route-argument syntax requires a separate language proposal.
A backward notification does not require an outstanding request/reply Future;
it does require an authorized upstream edge. `respond` is never a synonym
for `backward`. `done` closes only the current stage/turn, not the pipeline.

## Runtime-only PipelineContext

Pipeline route metadata is scheduler/coordinator issued, not guest-authored
and not authority inferred from a MessagePack schema ID or wire bytes. A
delivery context must bind at least:

- `pipeline_id` and `flow_id` (independent pipeline and in-flight flow identity).
- `message_id`, parent/causal identity, and optional reply correlation.
- Actor ref **scheduler domain, slot, generation/incarnation**, and the
  actor-turn's scheduler-issued continuation ticket.
- Actual ingress edge and authorized outgoing/reverse edges or opaque route
  capabilities, each bound to its endpoint actor generation and message type.
- Cancellation epoch, completion state, and bounded in-flight admission credit.
- A route/hop budget or equivalent cycle-policy guard; never recurse inline
  through actor callbacks to implement forwarding.

Reverse routing follows the actual ingress edge/causal route, not merely a
mutable `previousActor` pointer. For multiple ingress paths, the context
must preserve which branch produced this delivery. Route/capability tokens
are opaque and cannot be serialized by untrusted actors to acquire authority.

## Admission, scheduling and failure

1. Typecheck each directional value against the selected edge's destination
   **nominal variant contract**. Every accepted value owns a detached snapshot.
   Preserve the existing canonical wire data array
   `[2, schema_id, sha256(shape), payload]` without adding route information
   inside the application payload. Version routing metadata separately.
2. Reserve a bounded DATA/admission credit before allocating or decoding.
   Transfer it atomically on success; release it exactly once after the
   receiving turn finalizer, or immediately when rejected. `await` on
   backpressure suspends the guest continuation, **never** a scheduler carrier
   or the master OresVM control thread.
3. Define asynchronous result states separately: **rejected**, **admitted**,
   **delivered**, and **stage completed**. `forward`/ `backward` must not
   report delivery when only admission succeeded. A destination that stops
   must reject new delivery and settle pending send Futures with a typed
   failure or cancellation, not silently drop.
4. For a `serial actor`, HTTP requests and ordinary DATA messages share
   one logical lane; the actor's turn remains owned across `await` and any
   pipeline send admission barriers. No unrelated turn may enter until the
   suspended turn finalizes.
5. Reject missing/ambiguous routes, stale actor generations, stale
   continuation tickets, expired cancellation epochs, unauthorized reply
   correlation, wrong nominal type, and dead or stopped pipeline links.
   Duplicate/late success, failure, and cleanup callbacks cannot release
   another task's turn or queue credit. `defer` and `recover` execute
   exactly once on their authorized lane, with earlier failures preserved.
6. The compiler may not synthesize a callable `self.forward`/ `self.backward`
   fallback as an ordinary actor method while the runtime intrinsic is
   unavailable. Unsupported backend/lowering is a compile-time diagnostic.
   LLVM and Java/GraalVM each have explicitly bounded compiled three-actor
   nominal-int linear slices. This does not enable dynamic graph routing,
   arbitrary payloads, pipeline Futures, or unrestricted async callers.

## Compatibility and verification

- Existing Java generic tagged codec can handle up to **4,096** alternatives
  for codec-only use. The current **portable source actor binding ceiling is
  64 alternatives**, matching LLVM `SOURCE_MAX_VARIANTS`. Never imply that
  successful Java host-codec decoding alone establishes portable execution.
- Shape descriptor: at most 4,096 bytes; depth 64; frame at most 1 MiB.
  Retain existing exact v2 golden vectors and immutable-snapshot guarantees.
- Shared executable guest fixtures: 3-stage forward and backward; backward
  unsolicited notifications; source-vs-immediate-predecessor `respond`;
  fan-out/fan-in correlation; missing and ambiguous routes; stale ref after
  restart; cancellation during waiting admission; destination teardown;
  bounded mailbox overload; double completion; nested `await`; serial
  actor interleaving, and `defer`/`recover` error cleanup.
- Java: full Maven/JUnit and Graal native AOT/hybrid; LLVM: full CMake/CTest
  plus ASan/UBSan and optimized repeat executions. Validate exact Git tree
  SHA/blob identities for any funded CI mirror. Do not merge on codec-only
  or handwritten callback harness evidence.

## Verified first LLVM guest-execution milestone (2026-10-11)

- The source program `tests/fixtures/source-actor-pipeline-bidirectional.ores`
  defines three real actors, compiles their typed receive/match callbacks via
  LLVM, and uses `self.forward(Signal.Data(value))` downstream and
  `self.backward(Signal.Result(value))` upstream.
- A trusted zero-argument main binds exactly three distinct actors with
  `pipeline(source, transform, sink)`, sends one source message, and
  waits through `expect_pipeline(source, 19)` for the full owning-turn
  completion barrier. This is intentionally a single-flow, linear and
  synchronous *host-main completion* subset; actor turns themselves do not
  block on the downstream actor. No handwritten host-side actor callbacks.
- A scheduler-issued worker-local active actor context authorizes each
  directional emit. The destination must match the compiler-registered
  nominal MessagePack v2 contract; the existing exclusive DATA scheduler
  lane enforces mailbox ownership.
- Success requires **observed** two forward and two backward admissions,
  source/transform/sink processing of **2/2/1 turns**, the exact result
  **19**, and **zero outstanding DATA reservations**, checked only after
  every involved finalizer has completed.
- Exact-source funded CI
  [#38185810039](https://github.com/zed-pkg-test/java-app/actions/runs/38185810039)
  passed compiler building, actual guest LLVM IR generation, **100 native**
  executions, **30 ASan/UBSan** executions, and compile-time rejection of
  missing routing topology and unauthorized sink-forward calls.
- Not yet supported: generic dynamic fan-out/fan-in, actor-incarnation
  rebinding, bidirectional `Future<T>` admission, cancellation during
  backpressure, multiple in-flight flows, arbitrary payload shapes, or
  full original-repository build matrices.
  Do not treat the passing slice as proof of these semantics.

## Verified Java/GraalVM JVM guest-execution milestone (2026-10-11)

- The source program `examples/actor-pipeline-bidirectional.ores` defines
  three real Oreslang actors using the same `Signal.Data: int` and
  `Signal.Result: int` nominal cases as LLVM. It compiles with the
  Java/GraalVM evaluator and uses `self.forward()` and `self.backward()`
  in guest source, with no handwritten host callbacks.
- `pipeline(source, transform, sink)` binds three separate actor refs to
  a runtime-owned linear routing table. Every directional message uses the
  existing compiler-bound canonical MessagePack v2 decoder, detached
  recipient-owned DATA snapshot, bounded mailbox accounting, and serialized
  actor execution turn.
- The runtime checks actor/sender membership and a single-flow causal phase
  sequence: external source ingress, source forward, transform forward,
  sink backward, transform backward, then authorized source completion.
  Duplicate source ingress, direct middle-stage sends, stale/unbound route
  use and out-of-order or wrong-direction transitions fail closed.
- The Java binder's `closedDomainIdentity` now hashes the complete compiler-
  owned contract: qualified case names, numeric tags, and canonical v2 payload
  shape digests. Two implementations with the same names but conflicting tags
  or shapes are rejected before a route is bound. This changes **only route
  admission identity**, not canonical MessagePack schema IDs or existing
  Java/LLVM golden wire vectors.
- A failed root ingress poisons its single-flow phase reservation rather than
  leaving it apparently successful; directional sends after actor end/stop
  requests are fenced. This subset does not provide rollback/retry semantics
  or prove multi-flow admission linearizability.
- The typed guest source reaches final state **19** and ends all three actor
  lifecycles. The focused tests and entire **1,465-test JVM suite** (0 failures,
  0 errors, 0 skips) passed on GraalVM 25 in the exact-source funded run
  [#38191532323](https://github.com/zed-pkg-test/java-app/actions/runs/38191532323).
  The funded mirror checked 74 exact production Git source blobs.
- The funded [Native Image AOT and hybrid run #38191776758](https://github.com/zed-pkg-test/java-app/actions/runs/38191776758)
  built **both actual native executables** with GraalVM 25.3.4.1, inspected
  their capabilities, and successfully executed the same three-actor guest
  Oreslang program to produce `JAVA_PIPELINE_RESULT_19`. A mismatched
  GraalVM 25.0.4 run failed during Truffle class initialization; the
  dependency-aligned 25.3.4.1 run passed. Native guest execution is therefore
  verified for this **specific bounded slice**, not for arbitrary pipelines.
- The runtime binder remains a fixed single-flow, synchronous admission-only
  subset without general `Future<T>` pipeline suspension or graph fan-out/fan-in.
  There is no machine-checked implementation refinement proof yet between
  the two runtime lifecycle implementations.

## Bounded TLC safety evidence (2026-10-11)

- `contracts/v2/formal/LinearPipelineSingleFlow.tla` describes the six
  causal phases, the single pending admission reservation, terminal rejection,
  and source completion after five committed admissions. Safe and intentionally
  unsafe replay configurations are checked independently.
- Exact-model funded [TLC run #38192119587](https://github.com/zed-pkg-test/java-app/actions/runs/38192119587)
  passed all invariants after **17 distinct reachable states**. The deliberate
  duplicate-completion mutant produced the required `NoDoubleAdmission`
  counterexample. The checker artifact's SHA-256 was pinned and verified.
- This result is limited to the finite single-flow state machine. It is **not
  yet a Java/LLVM refinement proof** and does not model all scheduler
  interleavings, actor-generation ABA, multiple simultaneous flows,
  cancellation while awaiting backpressure, or branch correlation.

## Current admission status

Java/GraalVM now admits compiler-checked `self.forward` and `self.backward`
for nominal enum values, routed only inside actual actor mailbox DATA turns
of a runtime-bound three-actor pipeline. `self.respond` and `self.done`
remain unavailable. LLVM admits the corresponding fixed linear three-actor
`int` pipeline described above. Neither backend exposes unrestricted graph
routing or general pipeline Futures. Native Image guest execution is
verified for the fixed three-actor Java slice, but comprehensive cross-backend
lifecycle refinement remains unverified. No release readiness or general
actor-pipeline semantic parity is claimed.
