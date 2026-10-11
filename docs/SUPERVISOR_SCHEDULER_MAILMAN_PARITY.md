# Native scheduler, mailman and supervisor parity gates

This native runtime hardening slice introduces an **immutable, nonzero supervisor
identity** for each `NativeActorGroup`, established before any worker can be
admitted. The legacy two-argument constructor delegates to a root supervisor
identity; the explicit third argument selects a supervisor identity.

The identity is **not a capability**. Native host code must still validate
same-runtime supervisor registration at its privileged creation boundary,
before passing the ID to `NativeActorGroup`. A numeric ID alone must never
authorize termination, restart, panic interception or memory reclamation.

Current regression coverage: `tests/actor_group_lifecycle_test.cpp` checks
implicit root supervision, explicit immutable binding across group close,
and rejection of zero supervisor/runtime/capacity.

## Source parity is not yet complete

The intended spelling in *both* languages is:

```ores
define supervisor S as
end

define mailman M as
end

define scheduler X as
end
```

These are **nominal entities**, not classes or actor aliases.

The native frontend does not yet compile those declarations, source-level
supervisor policies, typed CONTROL callbacks, mailman continuation frames, or
source-defined scheduling policies. It must fail closed rather than emit
misleading functional native code. The Java compiler currently also lacks
distinct source-level declarations for these three entities.

The Java runtime counterpart exposes a root supervisor, explicit host-only
registrations, `createActorGroup(supervisor)`, and live advised-group snapshots.
Equivalent host registration and enumeration on native are outstanding.
The exact parity targets also include: bounded prioritized panic CONTROL,
actor-local throw/raise only after standard handling fails (never actor-local
panic handling), supervised group availability before any spawn/mailbox
admission, nonblocking scheduler-owned await/resumption, one serialized
bounded cooperative Mailman per ActorGroup, and closed-group/cancel fencing.

**Do not claim source-language parity from native host constructors.**

## Hierarchical supervisor actor direction (October 11 update)

The canonical source declaration is `define supervisor actor Name as ... end`.
An optional shorter `define supervisor Name as ... end` spelling can be
normalized only once nominal source lowering exists. `define proxy actor`
is a typed remote endpoint rather than a direct cross-process heap pointer.

`NativeSupervisorRegistry.hpp` now establishes a root, immutable parent/child
supervisor hierarchy, runtime-affine opaque handles, bounded native group
construction, weakly held group observation and direct/transitive group snapshots.
Its CTest `native_supervisor_hierarchy_group_binding` also rejects foreign
handles even when their raw numeric IDs collide. New source syntax is
explicitly rejected until compiler-checked CONTROL semantics exist.

See Java's detailed [OTP-inspired architecture proposal](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/main/docs/OTP_SUPERVISION_TREE_HOT_RELOAD.md)
(the linked path will exist on Java main after its accompanying PR merges).
In-place policy code replacement with safepoints, state migration, pinned
continuations and rollback is **not** implemented by this native registry.

## Typed actor pipeline parity extension

`ActorPipeContract.hpp` implements native host/compiler role admission for
readable/transform/writable/duplex stages with adjacent payload type checks.
It forbids SUPERVISOR and ordinary unmarked actors in all stage positions.
`native_actor_pipe_role_and_type_contract` is added to CTest. Java's
`ActorPipelineContract` mirrors the nominal stage compatibility rules.

Both frontends deliberately reject new source `define readable pipe actor`
and `spawn A() pipe spawn B()` syntax pending dedicated PipelineExpr
lowering, bounded directional data transport, explicit reverse reply
capability and the ActorGroup-supervisor admission transaction.
See Java's [typed pipeline contract](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/hardening/supervisor-group-binding-20261011/docs/ACTOR_PIPELINES_TYPED_SUPERVISION.md).

Pipeline actors are supervised *workers*; supervision trees do not
participate in normal forward-data routing, and an ordinary actor's reply
target is not automatically its supervising actor. Context code generation,
bidirectional replies, terminal shutdown and hot reload are future
runtime implementation gates, **not** proven by the admission test.

## Executable host transport and code-revision milestones

- `NativeActorPipeTransport.hpp` now creates a supervised ActorGroup before
  publishing a fixed set of bounded forward message edges. Per-edge capacity,
  bounded outstanding reader/writer waiters, a 1 MiB serialized-byte limit,
  close/cancel failure of pending futures, and no separate carrier threads
  are tested under strict native compilation and sanitizers.
- `NativeSupervisorCodeSlot.hpp` pins an immutable policy code revision for
  existing host CONTROL turns and atomically activates a subsequent revision
  after checking the expected revision/ABI. Code image cleanup waits for the
  final pin release. `NativeSupervisorRegistry` binds one slot to one
  supervisor in its own runtime-affine domain; changing code does not
  change child-parent registration or associated ActorGroups.
- These are host/runtime foundations. They do NOT implement executable
  source `define supervisor actor`, migration callbacks, pinned native
  *guest* continuation frames, native source `pipe` execution, upstream
  reply channels, or authentic cross-process proxy actors. Full CMake/CTest
  and shared source conformance remain merge gates.
