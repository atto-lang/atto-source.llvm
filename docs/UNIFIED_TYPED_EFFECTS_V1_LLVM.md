# Native LLVM typed effect gate v1

**Stage:** compiled-source typed IR opt-in, not a hard-real-time certificate, runtime authority, or complete AttoLang effect inference.

The Rust-hosted native `atto-llvm` bootstrap now accepts:

```sh
cargo run --bin atto-llvm -- input.atto -o output.ll --emit-llvm --effect-check=pure
cargo run --bin atto-llvm -- input.atto -o output.ll --emit-llvm --effect-check=deterministic
cargo run --bin atto-llvm -- input.atto -o output.ll --emit-llvm --effect-check=no-alloc
```

The gate operates on the `frontend::check::TProgram` typed instruction graph, after parsing and type checking but **before** LLVM IR lowering or destination creation. Direct calls are propagated by a finite recursive-cycle-safe worklist fixed point. The analysis recognizes scalar values, checked arithmetic (panic), conditional flow, runtime allocation, actor state, actor spawn/send, and printing. Any unsupported typed operation, unresolved target, imported library or hidden actor/generator/string/heap/exception runtime closure fails closed with `E_EFFECT_UNKNOWN`. The `no-alloc` label means only a conservative runtime-allocation screen, **not** a hard-RT proof; actor heaps, native allocation sites, WCET, stack bounds, runtime imports, interrupts, capacity and native ABI closure require separate evidence.

The earlier default `atto-llvm input.atto -o output` behavior is unchanged. The new effect flag does not grant capabilities, introduce runtime reflection/JIT, compile a guest interpreter, or create dynamic code.

Run focused Rust unit tests using `cargo test --bin atto-llvm effect_check -- --nocapture` and actual emitted native LLVM/compiled guest fixtures separately. No parser recognition of host clock providers is implied: unknown `clock.now` is a front-end rejection, not yet a typed `nondet` classification.

Versioned interchange target: [atto-common-source unified typed effect schema](https://github.com/atto-lang/atto-common-source/blob/feat/typed-unified-effects-gate-v1-20261121/contracts/analysis/UNIFIED_TYPED_EFFECTS_V1.md). The compiler currently infers an internal typed row but does **not** export this JSON envelope; integration/evidence remains outstanding. Native LLVM code generation, guest execution and NativeAOT/ASan/UBSan must be independently verified on the exact branch head; a zero-step CI failure is NOT_EXECUTED.
