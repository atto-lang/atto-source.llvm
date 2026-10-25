# Bounded dispatch ticket state model (Rust)

```sh
cargo test --offline --manifest-path tools/actor-dispatch-tickets/Cargo.toml
cargo run --offline --manifest-path tools/actor-dispatch-tickets/Cargo.toml
```

Ported from `scripts/verify_actor_dispatch_tickets.py`. Exhaustively enumerates
three ticket IDs, two workers, stopping, cancellation, completion, stale callbacks,
and issuance without ticket reuse. The negative mutant intentionally permits a
stale release and **must** produce a shortest-path counterexample.

This is a small, source-free reference model. It does not prove that LLVM
native actor scheduling uses the modeled transitions, nor that compiled actor
source executed, nor that production resource exhaustion is safe.
