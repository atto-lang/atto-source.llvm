# Correlated actor flow admission — implementation slice

Status: **host-owned state registry and native regression only; not enabled
in LLVM guest pipelines**. Stacks on generation-safe SourceActor ref PR #239,
which in turn stacks on MessagePack/pipeline PR #180.

## Invariants and trust boundary

- `SourcePipelineFlows.c` owns up to 128 independent flow slots. A slot may
  be reused, but the monotonically issued 64-bit flow ID never is. Each
  admitted hop gets an independent monotonically issued message ID.
- The tuple (pipeline ID, route generation, flow ID, causal parent, directional
  stage, destination actor ID + generation) is bound before publication and
  compared against the outstanding host-issued reservation. Guest MessagePack
  frame bytes remain `[2,schema_id,sha256(shape),payload]` unchanged.
- Cancellation and **nonblocking** physical DATA enqueue are linearized under
  the same registry mutex. The scheduler publish callback must itself validate
  current destination incarnation and reserve real bounded DATA capacity.
  It MUST NOT block, call registry APIs, run guest code or acquire the group
  mutex in the opposite lock order. When integrating, establish a total lock
  order or fold this linearization point into the existing group mutex.
- Cancellation affects one flow. Already admitted DATA does not retroactively
  disappear; late delivery must be fenced by a separate flow cancellation epoch
  when this table is integrated with scheduler-owned DATA turns.
- `finish` checks the expected number of admitted directional hops but **does
  not prove physical turn finalization**. The guest adapter MUST join finalizers,
  check reply ownership, and settle an actual typed Future before calling it.
- Flow IDs, source actor IDs, and route generations are **trusted host
  identities, not secret-bearing tokens**; never expose these C functions to
  untrusted FFI or infer authority from their numeric values alone.
- The table is not safe to destroy until all producers have been fenced and
  joined by the scheduler. Do not attempt stop-on-free from actor workers.

## Executable evidence

`tests/native_pipeline_correlated_flows_test.c` exercises shared routes with
80 concurrent active flows and 400 accepted typed-direction hop tickets, plus
200 cancellation-versus-publication races per run, wrong-direction/correlation/
generation/parent fencing, rejected DATA admission, full 16-stage 31-hop path
and 128-slot table saturation. This is a C host state-machine test, not
compiled Oreslang guest execution or actual MessagePack scheduler admission.

The TLA+ `ConcurrentPipelineFlows.tla` models two concurrent flow keys with
independent reservation, commit, cancellation and completion. Its two mutants
must produce counterexamples: late commit after cancellation and duplicate
admission without reservation. It is a bounded model, **not an implementation
refinement proof**.

## Follow-on integration gates

1. Replace the global `pipeline_phase`, `pipeline_root_sent`,
   `pipeline_done`, `pipeline_result` fields with per-flow scheduler-owned
   state while retaining the current exact cancel-versus-DATA publication
   barrier. Carry the immutable context in `SourceWireDelivery`, not guest
   payload bytes; bind it to the actual incoming scheduler DATA turn.
2. Make reverse routing use per-flow causal parent and actual incoming edge,
   not the mutable actor-wide `upstream` alone. Keep `self.respond` distinct
   from `self.backward`.
3. Require generation-fenced producer leases across admission, stop, restart,
   cancellation, and all bounded credits. Reject stale stop and stale reply.
4. Compile a real 3-actor `.ores` fixture with 1000 independent requests,
   interleaved upstream/downstream messages, selected cancellations, and typed
   replies. Verify exactly one logical completion per request and zero leaks.
5. Expand TLC to model scheduler credits, finalizers, Future suspension,
   generation reuse, and implementation trace refinement. Run full LLVM
   CMake/CTest, Java Maven/JVM and Graal AOT/hybrid differential suites.
6. Maintain exact Git blob verification when executing CI on a funded mirror;
   do not claim full release parity or merge these drafts beforehand.

Schema: `contracts/v2/pipeline-flow-context.schema.json`.
Companion TypeSpec: `contracts/v2/pipeline-flow-context.tsp`.
