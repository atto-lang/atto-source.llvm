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
