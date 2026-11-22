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

Versioned interchange target: [atto-common-source unified typed effect schema](https://github.com/atto-lang/atto-common-source/blob/feat/typed-unified-effects-gate-v1-20261121/contracts/analysis/UNIFIED_TYPED_EFFECTS_V1.md). The compiler currently infers an internal typed row but does **not** export this JSON envelope; integration/evidence remains outstanding. **Executed subset evidence:** the authorized private alternate-org runner [opto-sync/atto-ci workflow 44973071614](https://github.com/opto-sync/atto-ci/actions/runs/44973071614) first verified 19 original Rust compiler-module Git blobs, then built and ran 7/7 typed-effect tests, one legacy scalar IR test, and a real linked Linux x86-64 native Atto guest using a two-function checked arithmetic call chain. Each of the `pure`, `deterministic`, and `no-alloc` effect profiles produced native process **exit 42**. Unverified `stdio` was rejected without replacing a preexisting output file; unknown profiles were rejected before creating an artifact. The matching source-workflow test cases now live in `.github/workflows/typed-effects-v1-llvm.yml`.

**Limits:** the 19 verified compiler blobs and complete matching `frontend/src` subtree are **not** the exact full repository root tree; actor/FFI/network/clock closures, Java/native-image parity, ASan/UBSan and hard-RT/WCET are not verified. The source-org workflows remain zero-step infrastructure failures, never passing CI. No complete AttoLang backend parity, runtime capability grant or hard-RT certification is claimed.
