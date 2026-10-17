# Native fixed-state critical write-grant v0 — experimental

This draft is stacked on LLVM PR #264 (strict `with` bounds / lexical `using` state scopes), **not** current LLVM main. It addresses the specific native fixed-field write admission gap tracked by issues #424 and #426.

## What the new ABI enforces

- A bare `ores_actor_i64_heap_field_write` always aborts: possessing an actor turn alone is no longer mutation authority.
- Compiler-lowered synchronous state callbacks use `state_write_begin` / `field_write_granted` / `state_write_end` only when they contain assignments.
- One active write grant per heap; globally unique nonzero grant IDs never wrap/reuse; state writes require the exact active actor domain, turn epoch, matching grant ID, and mutable declared field.
- Nested grants and write upgrades while any heap borrow loan is outstanding are rejected.
- Leaving a turn and actor STOP revoke an abandoned write grant, so later turn/STOP reuse fails.
- The LLVM backend rejects its two legacy unscoped implicit field-mutation paths instead of continuing to emit native ungated stores.
- The positive native CTest verifies two successive turn-scoped writes; fork-isolated negatives exercise bare writes, missing/expired/replayed/foreign grants, immutable fields, borrow upgrade, nested grant, wrong turn and retire/leave revocation.

## Explicit limitations — no full security certification

- Native C/FFI host code and trusted scheduler entrypoints can call `state_write_begin`. Grant IDs are *lifetime tokens*, not cryptographic capabilities against compromised native code. The trusted runtime must own the entrypoint; guest source cannot access it.
- Compiler emission is a restricted serial fixed-i64 actor profile. The source parser/effect checker must prove every permitted lowering is a well-formed synchronous lexical critical scope. A runtime grant alone does not prove full source effect soundness.
- Initialization remains a distinct privileged transition to harden: `field_init` and `field_init_mutable` still authenticate a turn but not a constructor-only initialization phase. Do **not** mark constructor-state isolation proved.
- Cancellation/panic/throw/unwind must be tested through actual compiled guest paths; the runtime automatically revokes on `leave` and STOP, but an unexpected nonreturning callback needs trusted cleanup before next turn.
- The prototype does not yet attest TLS physical-carrier identity, ownership across arbitrary host FFI, all compiler-emitted stores, schema-version changes, multi-thread actor max>1 admission, or an unbounded formal invariant.
- No exact-head CMake/CTest, ASan/UBSan/TSan, LLVM guest execution, or Java/LLVM differential run has been executed on this branch in the source organization. A zero-step GitHub Actions result is `NOT_EXECUTED`, not green.

## Release requirements

1. Compile with C11 GCC/Clang warnings-as-errors, run `native_actor_state_write_grant_admission` and the full existing CTest suite; run ASan/UBSan and available TSan stress.
2. Run compiled guest state-callback fixtures and inspect LLVM IR to prove no call to the bare field-write symbol remains; verify begin/write/end dominance and no grant lifetime crossing await/select/yield/return/unwind.
3. Prove constructor-only fixed-field initialization has its own non-forgeable trusted phase, separate from ordinary turns and state writes.
4. Complete independent review, exact source commit/tree receipt and trusted CI runner-backed execution before considering a merge.
5. Keep #424/#426 OPEN and formal mutation-exclusivity **UNPROVEN** until runtime, codegen, all guest paths and actor lifecycle are jointly verified.
