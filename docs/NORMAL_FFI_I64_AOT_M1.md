# Normal-compiler native C i64 FFI — M1 restricted admission

**DRAFT: not a production FFI actor.**

Unlike the earlier M0 lexical prototype, this implementation takes an ordinary Atto source unit through the existing native Parser, function-type resolver, ownership checker, LLVM function declaration and AOT code generator. The only admitted foreign declaration is:

    extern fnc atto_add_i64(int a, int b): int using ffi;

The only admitted caller for now is an effect-qualified, zero-argument native main returning the approved foreign result directly with two literal i64 arguments:

    pub fnc main(): int using ffi {
        return atto_add_i64(20, 22);
    }

Build to LLVM IR via:
    oreslang-llvmc --target=host --ffi-grant=atto.math.i64.v1 file.atto

The binder is a fixed, reviewed external C symbol atto_add_i64(int64_t,int64_t):int64_t. The linking test supplies one separately compiled C object. The compiler never uses untrusted source to choose a library file, symbol alias, shared-object name or calling convention; linking without the symbol must fail. The emitted module preserves signed 64-bit operands/results. This test profile checks literal argument addition with signed 128-bit arithmetic before codegen so native C signed overflow cannot silently occur. The executable process entry wrapper still validates the result fits its exit-status ABI.

## Non-negotiable gates

- Source declaration requires exactly two value int arguments, int return and an explicit using ffi effect.
- The main caller must declare using ffi and return precisely the approved call. For now foreign arguments must be scalar integer literals, optionally negated. Nested calls, callbacks, other functions/return types and ownership paths are denied.
- A host compiler grant is mandatory. No source-only symbol definition, forged imports, arbitrary native library, pointer/slice, actor/class state, dynamic symbol or untrusted runtime path is admitted.
- Source modules/imports, wasm32, Cortex-M, native-hard-RT and incompatible experimental features are denied in this slice.
- Negative compiler cases must exit nonzero and write zero LLVM IR. A statically linked, unsatisfied foreign C symbol cannot produce a native executable.
- CI must compile the real LLVM18 compiler from an exact-source Git tree, then execute compiled foreign-call binaries under Clang/ASan/UBSan.

## Not yet implemented

- The CLI grant is an explicit *prototype* flag, not the required flags-2-env Deno-style production permission parser and not an authenticated actor-supervisor capability.
- The normal compiler still does not accept or instantiate a true FFI actor, use receives/sends mailbox typing, suspend for a foreign call, or send an actor supervisor CONTROL notice.
- No FfiError/Result downcall error channel, cancellation, generation-fenced lease, safe callback, arbitrary buffer ownership or signed artifact manifest is implemented.
- This is not Java/GraalVM, .NET NativeAOT or Forge parity, nor a proof of native 32-bit ABI execution.

## Promotion order

1. Introduce a compiler-owned FFI binding registry separate from raw source import symbols. Require target triple, deterministic artifact hash, signature hash and an authenticated supervisor-granted capability.
2. Bind language-level typed unary actor messages to scheduler-owned foreign work and checked Result<T,FfiError> reply; guarantee exactly-once completion, cancellation and retirement fencing.
3. Make flags-2-env / .cli-flags.toml the common permission parser for all native CLIs (CLI > env > TOML default), not a homegrown FFI flag parser.
4. Extend the scalar ABI to canonical bool and f64 then value/copy-only buffers. Re-run exact-source sanitizer suites, explicit native 32-bit when available, and cross-backend conformance.

Previous M0: static ffi-i64 C adapter proof in PR #22. M1 source branch/CI receipts live in the draft PR description and are not a substitute for a signed production authority.
