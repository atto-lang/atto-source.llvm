# atto-source.llvm

LLVM-based AttoLang compiler and native runtime for high-performance and real-time systems.

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

AttoLang is the public name; preserve existing Oreslang repository, CLI and `.ores` compatibility while specifying new portable source in `.atto`. Do not duplicate normative semantics from [atto-spec](https://github.com/atto-lang/atto-spec).

**Evidence gate:** record exact compiler/runtime head and tree, target/toolchain, nonzero executed steps, positive and negative guest results, and explicit passed/failed/unsupported/not-executed states. Host-only CI and `steps=[]` do not establish guest support. README scope is not a feature-completeness claim.
