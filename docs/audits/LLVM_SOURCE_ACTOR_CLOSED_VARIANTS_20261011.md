# LLVM closed source-actor MessagePack variants — October 11, 2026

Status: DRAFT. Native runtime conformance passes; general LLVM guest-language lowering is not complete.

## Implemented

The compiler-facing ABI now exposes oreslang_source_actor_spawn_variants and oreslang_source_actor_send_variant. The registry accepts 1–64 immutable MessagePack v2 contracts, copying and validating all schema IDs, shapes, and SHA-256 digests before actor registration. Duplicate schemas, malformed contracts, unsupported counts, and forged hashes fail closed.

Incoming DATA frames select an exact registered schema, then undergo full typed validation against a private owned snapshot before dispatch. The callback receives a compiler-local 0-based variant index; this index is NOT the source enum tag. The real Java-compatible tag remains inside the validated typed payload. All alternatives share the same NativeActorScheduler logical actor lane, mailbox capacity budget, and actor-owned state; scalar and one-schema entrypoints remain supported.

The source actor producer gate now closes admission on trusted stop and waits for already-entered send calls to finish validation/admission before releasing contracts. External callers MUST prevent new operations on opaque handles after stop begins. This producer lease is NOT a stale-pointer-safe or generation-checked public handle design.

## Verification

Funded exact-blob native workflow PASS:
https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38176845866

The exact-source run verified source Git object hashes and passed GCC -Wall -Wextra -Werror with ASan/UBSan, 20 repetitions of source-actor execution, the four-variant source inbox test, eight concurrent sender threads with 196 total messages, preserved single-flight state access, sender-buffer ownership, malicious and duplicate contract rejection, legacy scalar actor compatibility, existing one-schema ABI, the production Java Status.Message2 MessagePack golden, generic actor group adversarial admission tests, and optimized Clang repetitions.

Implementation: src/main/cpp/oreslang/runtime/SourceActorBootstrap.c and .h.
Regression: tests/native_source_actor_variants_v2_test.c and registered native CTest.

## Outstanding release blockers

1. LLVM currently parses nominal enum/pattern declarations, but compilation of typed guest sends, receive match, typed tuple destructuring, and exhaustive matches into the native variant ABI is not yet demonstrated. Direct C callback conformance is not a guest-source parity claim.
2. Java allows up to 4096 declared tagged alternatives; the conservative LLVM ABI currently supports at most 64. Align the portable limit or extend native indexing and resource budgets.
3. Producer leases cover entered calls, not arbitrary stale pointer use after native destruction. Enforce generation-checked handles or compile-proved ownership and shutdown.
4. Typed Future<T>, await/reply correlation, cancellation, defer/recover cleanup, and finalizer lifecycle still need executable cross-backend tests.
5. Full LLVM CMake/CTest, full Java Maven plus Graal native-image and long fuzz/stress runs on exact current integrated heads remain merge gates.

Keep PR #180 draft. Do not merge until the remaining release blockers have test evidence.
