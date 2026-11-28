# Native structured TaskScope lifetime gate

Status: runtime-private acceptance substrate. This change does **not** enable
source `task scope` / `nb scope` execution in the LLVM frontend.

The native gate exists to pin the lifecycle contract before compiler lowering
is allowed to depend on it.

## Required invariants

- Logical Future/completion terminal state is not physical quiescence.
- A registration retires only after both terminal authority and a scheduler
  physical-retirement witness have arrived.
- Normal scope close seals new root children. A physically executing live child
  may still admit descendants until terminal authority or quiescence revokes
  its permit.
- Cancellation seals all new descendants before revocation callbacks run.
- Failed admission is the only failure path that may retire without an attached
  completion identity.
- Completion identity is immutable: a different/stale terminal token cannot
  replace the attached runtime-created identity.
- Resource-like children may install an exactly-once scope-close signal, but
  that signal is not itself a retirement witness.
- Native TaskCancellation authority retires only at the same terminal +
  quiescence conjunction.

## Current integration boundary

`StructuredTaskScopeGate.hpp` composes with `TaskCancellation` but is not
yet connected to source Task lowering. Source execution must continue to fail
closed until the frontend can prove:

1. exact checked TaskExpr / scope admission;
2. submit-time ownership transfer;
3. runtime-created child/completion identity;
4. continuation parentage across every suspend/resume;
5. final cleanup before physical quiescence;
6. cancellation and scope-close fencing;
7. no actor TaskScope enablement before actor quota, arena, supervisor and
   hot-reload epoch lifetime integration is independently proven.

## Acceptance evidence

`tests/native_structured_task_scope_gate_test.cpp` covers:

- terminal-only and quiescence-only non-drain;
- both witness orders;
- failed admission;
- stale completion identity rejection;
- root sealing plus live-child descendant admission;
- cancellation sealing before physical retirement;
- late exactly-once scope-close hooks;
- close-vs-descendant races;
- cancellation-vs-admission races.

The test is part of `scripts/test-native-task-continuation-gate.sh` in both
normal and sanitizer modes.

A workflow that does not execute these compiler/test steps is not conformance
evidence. Source-free models may validate the abstract invariant but cannot
certify this native implementation.
