# ActorRegionV2: bounded multi-owner cleanup across a guarded return

**Stage:** opt-in compiler-backed LLVM guest slice. It does not certify
arbitrary CFG cleanup, exceptions/cancellation, live source actors, general
runtime AOT or process-wide zero-GC.

Command:

```sh
oreslang-llvmc --actor-region-v2-guest --target=host main.atto
```

The production parser, module resolver and ownership checker run before LLVM
generation. The first ActorRegionV2 cleanup planner admits **1–8 fresh,
immutable, lexically owned instances** of one class with 1–16 public, final,
literal integer fields. A single pure equality guard has an early scalar
field return and one normal scalar field return.

The compiler creates an `ActorRegionCleanupPlanV1` separately from LLVM
emission. It records each unique local owner in declaration order, seals the
supported normal-exit set, and constructs a reverse-order destruction list.
The code generator allocates one authenticated reference sidecar per owner,
emits the runtime allocation and initialization for each, and checks every
field read against the corresponding original sidecar. Both branches converge
on one LLVM cleanup block. The selected scalar result is snapshotted in a PHI
before the planner's reverse-order drops, turn leave, retirement and checked
region destruction.

The planner is a **bounded cleanup plan**, not yet a general dataflow solver:
it currently admits exactly one guarded early return and one final return.
It does not infer drops for arbitrary loops, nested scopes or exception edges.
A future exit kind must be added to an explicit, validated exit set before
it can be admitted. Throw, recover, defer, await, cancellation, actor
termination callbacks and FFI pins remain rejected.

## Native tests

The registered `native_actor_region_v2_multiowner_guarded_cleanup` CTest
must compile three real `.atto` programs: the guard-taken and guard-not-taken
two-owner paths, plus a three-owner path. It checks that the emitted LLVM IR
has the expected number of native allocation/drop calls, screens the module
with the existing no-GC IR auditor, links the real `ActorRegionV1.c` and
`ActorArena.c`, and executes each guest at Clang `-O0` and `-O2`.
Eleven negative cases check that owner counts above eight, mutable/borrowed
owners, unmodeled roots/guards/calls and exit-status truncation reject before
LLVM IR/artifact output.

The test is **authored and registered, not passed**, until executed on an
exact source head with nonzero CI steps and retained native binary hashes.
An IR string or host-only model is not guest execution evidence.

## Unsupported and future work

- More than eight owners, arbitrary numbers of CFG exits, nested scopes,
  loops and branch-local ownership.
- Class methods, dynamic dispatch, constructors with arguments, generics,
  imports, macros, actors, tasks, await, exceptions, cancellation and FFI.
- Real scheduler-issued actor identity/turn authority and supervisor lifecycle.
- Transitive runtime/FFI link certification, a controlled guest fault boundary,
  complete linked-process no-tracing-GC proof, and Java/.NET/Wasm/Forge parity.

All existing compiler and runtime implementations are preserved. ActorRegionV2
is opt-in and does not replace V1 or legacy LLVM lowering. Strict sealed runtime
admission remains independently gated by LLVM PR #1590.
