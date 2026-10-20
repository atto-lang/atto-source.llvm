# LLVM native actor model-checking gate

The fast model-only workflow in this repo exercises existing deterministic, bounded, stdlib-only Python explicit-state explorers. This is a separate gate from the larger native sanitizer and compiler builds.

- scripts/verify_actor_carrier_exclusivity.py explores physical leases for two actors and two workers, including await / resume / cancel / stop interleavings. A deliberately unsafe double claim must yield a multiple-carrier violation.
- scripts/verify_actor_dispatch_tickets.py explores stale dispatch release and stop with two workers and three tickets. A deliberately unsafe stale release must yield a counterexample.

The workflow fails if a checker fails, and records its exact checkout commit/tree. Its path trigger includes the native scheduler and authority files, so relevant runtime changes cannot avoid the model-only job by leaving the model scripts unchanged.

IMPORTANT: These are ABSTRACT bounded models. Passing does not prove that compiled LLVM machine code refines their transition systems or satisfies zero-GC/fully-AOT rules. Full native execution, concrete trace conformance and exact-head Actions runs remain separate admission gates. In particular, do not confuse a stale dispatch-ticket check with the generation-fenced ActorRef/restart protocol.

Cross-repo next step: implement real actor/supervisor DATA/CONTROL trace observation (admission, physical enter/leave, fence, cancel/stop, restart, old-generation rejection) and compare it to the model in oreslang-platform-conformance PR #121. To prove the source-level contract, also execute Oreslang guest fixtures with exact binary+source tree identity and negative mutations. No backend certification is implied by this workflow.
