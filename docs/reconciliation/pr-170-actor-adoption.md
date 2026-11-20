# PR #170 — atomic native family-adoption forward-port

Historical head 3a231a0f1d6e3ec9c370a2f39837da6dc570ec19; current LLVM main 6ea1af47b97fe24a1d5e09e8feba1a8d67b03a5a.

Current-main `ActorGroupLifecycle.hpp` remains the lifecycle authority: preserve its resource-loan quiescence, mailbox/CONTROL, generation and physical retirement fences. Add only atomic `adopt_child(parent,...)`, read-only `spawned_by(child)`, and a shared private admission implementation. Under the original group+registry lock pair, parent generation/ticket validity, stopping state, group closure, child capacity and single ownership are checked before admission. Spawn lineage is diagnostic only and grants no management privilege.

Standalone `native_actor_child_adoption` CTest exercises root/child/grandchild lineage, foreign-group refusal, capacity fencing, close-driven stop, and close admission denial.

**Deferred:** original historical supervisor-policy and panic-control fields/callbacks cannot replace modern host CONTROL; source-level spawn/recover and Java/LLVM parity remain unverified. Require exact-head compiler CTest, sanitizer and concurrency validation. Git mergeability is not a runtime release gate.
