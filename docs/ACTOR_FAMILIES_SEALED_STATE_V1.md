# Oreslang actor families: sealed state, specialization, inheritance and capability contracts (v1)

**Decision target — 2026-10-14. Status: architecture + conformance specification, not a claim that these classes, source syntax, hot swapping or all guest pipelines execute in either compiler.** This pre-1.0 design must be compiled and tested on Java/GraalVM and LLVM before a feature is advertised as implemented.

## Goal and limits

Build one actor runtime that supports Erlang/Gleam-style fault isolation, Rust-style ownership and type safety, high-throughput bounded streams, cyclic/acyclic graphs, deterministic discrete-event and continuous simulation, distributed and data pipelines, and AI/ML/LLM orchestration. These are workloads, not reasons to create independent execution runtimes, unbounded threads per actor, or a new actor object model.

**Invariants (normative):**

1. An actor is a specialized class (one nominal class/object model); the compiler records its actor kind and preserves actor-only lexical isolation. An ordinary class must not become an actor merely by inheriting an actor name. No unvalidated cross-kind inheritance.
2. Exactly **one effective** guest-declared `state { ... }` schema exists per concrete actor instance. All guest-owned persistent instance data is stored there. Ordinary actor fields, inherited fields, hidden user side tables, mutable static fields, constructor-captured state and other persistent storage escape hatches are forbidden. Compiler/runtime-owned mailbox, tickets and lifecycle metadata are separate, inaccessible implementation internals.
3. Only the non-overridable `self.handle_state(callback)` intrinsic or its equivalent **lexical** `using state.read(|s| -> ...)` / `using state.write(critical |s| -> ...)` can produce an ephemeral typed state loan. No `self.state` access, method override, reflection/FFI cast, alias capture, loan return or cross-actor state mutation.
4. `with` on a callable signature declares the *upper bound* of its effects; `using` inside a body *acquires* a scoped capability and is never a signature effect declaration. `critical` requests exclusive state-write admission, not a physical mutex. Read callbacks are immutable unless explicitly granted write.
5. One native thread at most runs a given actor's guest code at a time. Logical message lifetimes may overlap through admitted suspension, but no borrow/loan may survive a conflicting yield, select, cancellation or reentrancy boundary. An awaited callback does not hold an OS mutex or promise transaction-wide atomicity.
6. Actor interaction is through typed messages/streams, not public user method RPC. Intrinsics `handle_state`, lifecycle operations and `self.forward()`/`self.backward()` are sealed, runtime-owned and cannot be overridden. The supervisor CONTROL plane is not a user-data route.
7. Admission is bounded by messages **and bytes**, with cancellation, teardown, generation/epoch fencing, terminal errors and exactly-once release of owned resources. No actor-per-OS-thread requirement; no raw cross-isolate pointer transfer.
8. Actor kind (SHARED, PRIVATE/ISOLATED, UNTRUSTED) determines execution and capability policy, **not** a substitute for type/borrow checks. No inherited method or trait can grant privileges missing from the concrete actor's capability manifest.

## Actor hierarchy: one intrinsic + library abstractions

`Actor` is compiler/runtime intrinsic. Shared, private and untrusted actor **kinds** are trusted profiles; they need not be ordinary user-instantiable superclasses. User inheritance remains nominal, single concrete actor ancestry, kind-compatible and stateless in the v1 parent layer.

Proposed abstract/library actor families, **not shipped built-ins**:

| Family | Responsibility | Preferred contracts (interfaces/capabilities) |
| --- | --- | --- |
| `SupervisorActor` | Child lifecycle policy and typed CONTROL decisions | `Supervises`, `Restarts`, `HealthObservable` |
| `StreamActor<I,O>` | Bounded streaming and EOF/error propagation | `StreamSource<O>`, `StreamOperator<I,O>`, `StreamSink<I>` |
| `PipelineActor<I,O>` | Typed forward/backward pipeline graph participation | `PipelineStage<I,O>`, `BackwardRoute<R>`, `Checkpointable<S>` |
| `GraphActor<M>` | Membership, routing, graph versions, graph traversal | `Routable<M>`, `TopologyObserver` |
| `SimulationActor<E>` | Virtual-clock simulation orchestration | `DiscreteEvent<E>`, `ContinuousSystem<S>`, `CoSimulationBridge` |
| `DataActor<I,O>` | Ingestion, ETL, partitioning, windows and durable sinks | `Partitioned<I>`, `Windowed<O>`, `TransactionalSink` |
| `ModelActor<I,O>` | ML model loading, batching, inference/training coordination | `Batching`, `ModelVersioned`, `Checkpointable<S>` |
| `LLMActor<T>` | Token generation, session routing and model orchestration | `TokenStream<T>`, `KVCacheOwner`, `InferenceEngine` |
| `RemoteActor<M>` | Location-transparent *typed* remote protocol endpoints | `VersionedWireProtocol<M>`, `IdempotentReceiver` |
| `ComputeActor<I,O>` | CPU/GPU/accelerator job submission and resource leases | `DeviceLeaseOwner`, `ComputeKernel<I,O>` |

These are largely *abstract behaviors plus library composition*, not ten scheduler subclasses. A concrete `LLMActor` can participate in a `PipelineStage` and satisfy a streaming interface without multiple concrete state-bearing inheritance paths. Performance-critical tensor kernels and numerical solver inner loops run in compiled compute kernels/owned data regions, **not an actor/message hop per scalar or per tensor element**.

**v1 state inheritance rule:** abstract actor parents are stateless behavior providers; the concrete leaf declares the sole `state { ... }` schema. No parent/child schema concatenation, shadowing or implicit reinitialization. An abstract parent may be generic over an interface for data behavior, but cannot close over a concrete child state loan. A future explicit, compiler-verified schema-extension facility would require stable field IDs, migration and disjointness proofs before enabling it.

## Illustrative source (not executable until inheritance and family contracts lower in both backends)

```ores
// Proposed abstract actor family; source grammar is provisional.
define abstract actor TransformStage<Input, Output> as
    // No persistent fields, no state block in an abstract parent.
end

define actor NormalizeStage from TransformStage<Batch, Batch> as
    state { processed: int = 0; }

    receive(ActorMail<Batch> mail): void with state.write, actor.forward {
        const next = normalize(mail.value);
        using state.write(critical |s| -> {
            s.processed = s.processed + 1;
        });
        self.forward(next); // downstream, typed, bounded, runtime-owned
        return;
    }
end
```

The `actor.forward` effect name and abstract actor family grammar are proposed and must be standardized; this is **not** a passing parser example. `self.backward()` is a distinct upstream-directed typed operation. The runtime binds each message to a pipeline/topology ID, direction, ingress/response correlation, generation, and credits. It must not infer direction from payload shape; looping messages require hop/fuel bounds or an explicit allowed-cycle contract. Forward and backward channels retain independent ordering/flow-control and explicit end/error semantics.

## Specialized contracts

### Fault tolerance and hot deployment

Supervisors own CONTROL decisions and children belong to explicit managed actor groups; inherited spawn lineage is *not* automatic authority to supervise. Support restart strategies (one-for-one, rest-for-one, one-for-all) only with bounded restart intensity, backoff, terminal escalation, poison-message handling and typed failure receipts. `throw`, `raise`, actor-fatal `panic`, cancellation and host-fatal errors remain distinct. Cleanup and DONE barriers must finish before ownership is recycled.

Hot deployment is a **proposed protocol**, never replacing code from underneath a live instruction pointer: quiesce ingress; reach safe point; snapshot actor-owned state under loan/revocation rules; verify source/target schema IDs and signed module identities; run an explicit bounded/typed migration; atomically switch code+state incarnation and routes; resume or roll back without delivering to both epochs. In-flight messages need versioned envelopes, deduplication and an explicit drain/rejection policy. A restart is not automatically a transparent upgrade.

### Streams, graphs and data/AI pipelines

Each edge has static `Input`/`Output` message domains, single-consumer or explicit fan-out semantics, bounded item/byte credits, cancellation ownership, and deterministic EOF/error release. Distributed delivery should expose its guarantees: at-most-once, at-least-once plus deduplication, or transactional application-specific protocols; do not claim universal exactly-once delivery through crashes/partitions. Graph rewires are versioned, admission-fenced topology transactions.

Data/ML stages add typed tabular/array/tensor shapes, zero-copy only for proven compatible lifetime/ownership domains, device leases, deterministic seeds when requested, and explicit host/device transfer effects. Model weights can be immutable/shared where permitted; session/KV-cache state remains owned, quota-limited and migratable only by an explicit protocol. No unsafely inherited FFI/GPU authority in untrusted actors.

### Discrete and continuous simulation

Discrete-event engines own a virtual event clock, stable tie-breaker and deterministic queue ordering. Continuous-time solvers own numerical integration state, step-size/error tolerances and checkpoint boundaries. Their coupling is an explicit co-simulation bridge with time-window and causality contracts; no implicit assumption that wall-clock scheduling order is a valid simulation clock. Replay records inputs, RNG seeds, versions and admitted nondeterministic effects.

## Mandatory static negative cases

Reject: ordinary actor instance field, mutable actor static, actor constructor capturing persistent data, multiple effective `state` blocks, state schema shadowing, actor inherits ordinary mutable class, cross-kind actor inheritance, direct `self.state`, forged `handle_state`, overriding a sealed intrinsic, read-to-write escalation, `using` as signature effects, declared `with` underruns, leaked state loans, suspension with live state access, public user method remote invocation, untyped forward/backward routes, unauthorized device capabilities.

## Development order and measured acceptance

1. **Foundation:** compiler-normalized actor kind + stateless abstract actor inheritance; reject *all* foreign persistent fields through inheritance and imported symbols; require exactly one concrete schema; verify `with` / scoped `using` / `critical` in Java and LLVM. Preserve current runnable subsets and fail closed on unsupported extensions.
2. **Typed dataflow:** `StreamOperator`, `PipelineStage`, `self.forward`, `self.backward`, bounded multi-edge flow control, graph versioning, 3-actor bidirectional guest execution and 15-stage stress in both backends.
3. **Resilience:** supervisor error taxonomy, restart policy, checkpoint/load and incarnation-fenced schema migrations; cancellation/restart/upgrade race properties.
4. **Simulation + data:** deterministic discrete-event replay, continuous solver and co-simulation; typed dataset layouts, windows and snapshots with fault injection.
5. **AI/ML + distributed:** actor-mediated model serving, owned KV cache, tensor/accelerator leases, multi-node protocol, durable receipts; throughput/latency/memory baselines relative to equivalent Rust/Erlang/Python workloads.

A gate is complete only with **the exact Git tree** under test, actual compiled Oreslang guest execution on both Java/GraalVM and native LLVM where applicable, negative source diagnostics, sanitizer/Native Image coverage where applicable, fault-injection and bounded resource tests. Documentation, Java-only host tests, or copied public mirrors of private source do not qualify as comprehensive parity. Private test mirrors must respect the repository privacy policy and use only explicitly authorized private destinations.

See `contracts/v1/actor-family-conformance.json` for machine-readable target cases. Both repositories must carry the same manifest; statuses remain DESIGN until executable gates land.
