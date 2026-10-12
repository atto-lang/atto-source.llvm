# Actor scalar StateAccess — LLVM native runtime parity gate

## Purpose

The Java/GraalVM [source actor native state stack](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/648)
now lowers fixed `state { ... }` scalar keys into actor-incarnation native
memory and executes `self.handle_state` under one logical CriticalGate.
This C++17 native runtime substrate establishes the matching **lifetime
contract**, but does not claim LLVM frontend support for that source syntax.

## Native lowering contract

- `CriticalGate::Token` is owned by the scheduler/continuation, never exposed
  to guest code. It can survive await and migrate across physical carriers.
- `OresArenaTurn` is a physical-carrier lease. Leave it before migration;
  the next carrier enters a new epoch and may reopen state only after admission.
- `NativeScalarState::Scope` is the lexical mutable field loan, not the gate
  token. It validates the ticket, the current active view and arena turn at
  every read/write. It cannot cross `await`/yield: the lowering calls
  `assert_can_suspend()` first.
- Nested scopes under the same token fence outer views and restore them on
  exit. A prematurely closed ancestor is not restored after nested cleanup.
- Cancellation/teardown `retire()` revokes all future view operations;
  physical storage survives retirement until native loans drain. The runtime
  frees/zeroizes the actor arena only after quiescence.
- Dynamic `Map<K,V>`, class/reference projections and memory-segment exposure
  are **not** admitted until compiler-owned scoped projection types exist.

## Verified

- `tests/native_scalar_state_test.cpp`: native initialization/persistence,
  nested mutable loans, illegal yield, FIFO gate handoff, queued cancellation,
  stale owner rejection, carrier migration, retirement, outstanding loan
  quiescence and expired ancestor handling.
- [Funded exact-blob CI #38169408002](https://github.com/zed-pkg-test/java-app/actions/runs/38169408002):
  strict C11/C++17 compile and native executable **passed**, plus AddressSanitizer
  and UndefinedBehaviorSanitizer **passed**.

## Remaining release gates

1. LLVM parser, type checker and codegen must admit the same fixed scalar actor
   schema and sealed `handle_state` source as Java, with a shared negative
   fixture matrix for escaping borrows and suspension.
2. Scheduler lowering must own critical tokens and call
   `NativeScalarState::assert_can_suspend()` before every physical handoff.
3. Cross-actor and cross-incarnation serialization, cancellation/panic cleanup,
   allocator metrics and forced scheduling races need end-to-end native tests.
4. General typed Maps and reference layouts need scoped projections and
   generation-checked stale-handle rejection in both backends.

Until those items are implemented, keep unsupported LLVM source constructs
**rejected** rather than silently compiling them with missing isolation.
