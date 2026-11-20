# Rust-only native Atto source-actor execution gate

`tests/source_actor_e2e.rs` ports the existing 16 positive compiler→LLVM IR→Clang→native actor execution fixtures and all 49 negative source-to-no-IR diagnostics from the older `source_actor_e2e.py` harness. It must be compiled using Rust's compiler; it does **not** invoke Python, execute a Python test helper, or mock the Atto compiler.

For an approved, exact-head LLVM source tree with a built `oreslang-llvmc` and Clang:

```sh
rustc --edition=2021 -O -D warnings tests/source_actor_e2e.rs -o /tmp/atto-source-actor-e2e
/tmp/atto-source-actor-e2e --compiler /absolute/build/oreslang-llvmc --clang /usr/bin/clang --root /absolute/llvm/source
```

The runner rejects missing inputs, unexpected compiler IR, nonempty stdout on rejected sources, compiler diagnostic mismatches, wrong native exits, missing runtime actor/await receipts, ownership/shutdown mismatches, and timeouts. It checks the original 16 live-actor fixtures and 49 negative fixtures and prints the historic terminal PASS receipts **only after all validation**.

`rustc --test` validates the runner's input inventory and receipt/ownership oracle **without running a guest**. **Do not cite that as native compiler execution.** The legacy `.ores` fixture names must ultimately migrate to reviewed `.atto` under the language-spec migration; this port does not authorize changing accepted syntax, changing actor behavior, or claiming any Redis/Kafka/NATS/RMQ protocol support.

The old Python harness remains for historical comparison until a full exact-source Rust guest run is verified and maintainers explicitly retire it. Do not run it in Rust-only broker acceptance workflows. All host source and binary SHA claims must be independently bound to checked-out PR HEAD/tree; this Rust runner does not authenticate the toolchain path.
