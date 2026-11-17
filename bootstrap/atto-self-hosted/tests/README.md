# Test protocol

`scalar_seed_model.rs` is an **independent Rust host reference**, not a
test of the Atto-authored guest source. Use it to validate expected Wasm
instruction bytes and rejection vectors while the native bootstrap
backend is extended to compile `src/scalar_codegen.atto`.

Run without Cargo dependencies:

```sh
rustc --edition 2021 --test bootstrap/atto-self-hosted/tests/scalar_seed_model.rs -o /tmp/atto-seed-model
/tmp/atto-seed-model
```

The next gate must compile **the actual Atto guest code** with a pinned
native Atto compiler, run its entry and test multiple external inputs.
Do not mark language acceptance passed from Rust-only reference tests.
