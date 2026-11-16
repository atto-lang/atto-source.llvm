# Bounded actor carrier exclusive-admission model (Rust)

Run `cargo test --offline --manifest-path tools/actor-carrier-model/Cargo.toml`
and `cargo run --offline --manifest-path tools/actor-carrier-model/Cargo.toml`.

This mirrors the source-free Python model with two actors, two workers,
two reentrant jobs for actor A, and one job for actor B. The safe state space
contains 372 states and produces a real suspend-then-resume interleaving.
An intentionally unsafe double claim is detected after 65 states with a
two-step counterexample.

The model is **not** a proof of production actor scheduling, native code,
actual work-stealing, or cross-backend runtime safety. The Python model is
retained as a comparison oracle until both execute on an authorized runner.
