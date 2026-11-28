> **AI agent instructions (organization-wide):** Before working on code, issues, or pull requests, read and follow the [organization `AGENTS.md`](https://github.com/atto-lang/.github/blob/main/AGENTS.md), including its **merge-first stealth-development policy**. Honor any repository-local `AGENTS.md`, explicit holds, and GitHub-enforced rules.

# atto-source.llvm

LLVM-based AttoLang compiler and native runtime for high-performance and real-time systems.

## Naming and specification migration

This is the canonical [AttoLang backend repository](https://github.com/atto-lang/atto-source.llvm). Use `.atto` for portable AttoLang source and update local remotes to `https://github.com/atto-lang/atto-source.llvm.git`; do not use historical repository links or naming in new examples.

- **Normative grammar and semantics:** [atto-spec](https://github.com/atto-lang/atto-spec). Check the current versioned contracts for ownership operations (`borrow!`, `take!`, `share!`, `copy!`, `copy!!`), implicit argument borrowing, `lt!` binders, `eq`/`neq` value equality, `susp` effects and file/module `define const`. These are *specification targets*, not claims of executable support in this backend.
- **Positive and negative guest execution evidence:** [atto-platform-conformance](https://github.com/atto-lang/atto-platform-conformance) and [atto-compilers-parity-testing](https://github.com/atto-lang/atto-compilers-parity-testing). Report `passed`, `failed`, `unsupported` or `not executed`, and pin exact compiler, source tree, target and toolchain.
- **Portable shared sources and target constraints:** [atto-common-source](https://github.com/atto-lang/atto-common-source) and [atto-target-profiles](https://github.com/atto-lang/atto-target-profiles). Keep backend-specific gaps explicit, and do not infer guest parity from host tests or zero-step CI runs.

## Scope

This repository is the AttoLang ecosystem home for:

- LLVM lowering, ownership-aware runtime semantics, native actor isolation, and target-specific code generation.
- Compatibility with the [canonical language specification](https://github.com/atto-lang/atto-spec) where applicable.
- Evidence-based tests, benchmark results, and cross-backend conformance reporting.

## Architecture and compatibility

AttoLang is designed around actors, explicit ownership, and portable concurrency semantics. Backend-specific optimizations must not silently change the shared language contract. Runtime capabilities, target restrictions, and unsupported features should be documented explicitly.

## Building and testing

Consult the checked-in source tree and CI workflows for verified commands and supported targets. Add target-specific build instructions and exact-head execution results as they become available; do not treat a passing host-unit test as proof of guest-language conformance.

## Project status

This README describes the repository's intended responsibilities. Feature support and release readiness must be established from implementation, tests, and CI evidence rather than this description alone.

## Related repositories

- [AttoLang organization](https://github.com/atto-lang)
- [Language specification](https://github.com/atto-lang/atto-spec)
- [Platform conformance](https://github.com/atto-lang/atto-platform-conformance)
- [Commercial infrastructure](https://github.com/atto-co)

## Repository ownership and contribution routing

**Belongs here:** LLVM frontend/lowering, native code generation, ownership and borrow checks, tenant/actor heap and stack ABI, native toolchains and sanitizer-backed execution.

**Belongs elsewhere:** JVM, .NET and Wasm backend-specific lowering belongs in their respective atto-source.* repos; target declarations live in atto-target-profiles.

**Security and portability boundary:** Bound allocations/indices and 32-bit actor heap offsets, fence tenant generations and overflow, contain actor stack traps to the owning actor, and prove real native guest execution.

AttoLang is the canonical project identity. Use `.atto` source, `atto-*` package and repository names, and the active AttoLang CLI contract for current documentation. Do not duplicate normative semantics from [atto-spec](https://github.com/atto-lang/atto-spec).

**Evidence gate:** record exact compiler/runtime head and tree, target/toolchain, nonzero executed steps, positive and negative guest results, and explicit passed/failed/unsupported/not-executed states. Host-only CI and `steps=[]` do not establish guest support. README scope is not a feature-completeness claim.

## Experimental Rust-hosted native AOT and actor slice

The separate `Cargo.toml`, `frontend/`, `src/main.rs`, `src/target_native.rs`, and `runtime/` path stages an **experimental bounded native compiler and actor runtime** alongside the existing checked C++ LLVM compiler slice. It is not a replacement for the canonical language specification or a claim that both compiler pipelines have identical feature coverage.

For the supported host profile, build the Rust-hosted compiler with `cargo build --offline --release --bin atto-llvm`. The source-owned smoke and actor regression scripts are `ci/test_native_aot.sh` and `ci/test_native_actors.sh`. See [native-AOT limits](docs/native-aot.md), [actor runtime scope](docs/native-actors.md) and [receiver method / sealed-state scope](docs/native-mtd-critical.md).

The independent [exact-head private CI evidence](https://github.com/athlet-o/athleto-e2e/actions/runs/43669427959) covers the original isolated Rust-hosted compiler snapshot: compiled native actor scalar cases, negative compiler admissions, sanitizer checks, and protocol scalar fixtures. **That run does not certify the subsequently combined mainline tree.** In particular this slice does not implement complete actor supervision, full ownership across suspension, general FFI, QUIC/TLS/QPACK networking or five-backend parity. The HTTP/3 ABI contract is a proposed interface, not a working network engine.
