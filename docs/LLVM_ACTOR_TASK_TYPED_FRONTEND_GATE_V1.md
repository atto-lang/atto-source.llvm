# LLVM native actor Task/await — typed frontend admission gate

Status: **typed source AST only; compiler emits an explicit error; NO compiled guest actor Task execution**.

This change is stacked on the Native VM TASK/WAKE bridge (#1731), whose dependencies are actor TASK lane #1724 and unsafe foreign Task pool rejection #1715. Parallel host/runtime proof #1734 provides a separate actor-owned WAKE lifecycle candidate. Do not duplicate or silently replace that bridge with the legacy `SourceTaskRuntime.c` one-actor synthetic pool.

## First restricted source form

```atto
define actor Worker as {
  let total: int = 0;
  pub suspends fnc on_message(int message): int {
    let Task<int> work = task { return 40 + 2; };
    return await work;
  }
}
```

The parser stages exactly **one** `Task<int>` binding; its child producer is a capture-free, scalar-only expression. The handler uses an explicitly declared `suspends fnc` or `suspends mtd` effect and returns the result of exactly one `await` on the same owned task. No `self`, field, handler-parameter, borrowed object, closure, runtime call, nested task, or compiler-created implicit observer is admitted. The frontend separately validates provenance and i64 result type.

**The AST is not executable yet.** `LlvmCodegen::emit_verified_llvm_ir` checks the marker before creating LLVM IR and raises:

```
ATTO_TASK_E_ACTOR_LOWERING_PENDING
```

The AST carries an inert placeholder only to keep existing nonnullable visitor invariants; it must never reach codegen or be misreported as a valid guest result. Both native and wasm32 LLVM emission are guarded. Synchronous actor handlers may not contain `task`/ `await`; they receive `ATTO_TASK_E_SUSPENDS_REQUIRED`. A non-capture-free producer, mismatched awaited task, missing Task binding, or incorrect result type receives a distinct diagnostic.

## Real compiler tests, currently NOT_EXECUTED

Seven `oreslang_contract_test` negative cases are wired into CMake. They invoke `oreslang-llvmc` and require exactly exit code 1, the named semantic diagnostic on stderr and **zero stdout/IR**:

- `native_actor_task_await_staged_no_ir` and `native_actor_task_await_method_staged_no_ir` — typed forms cannot emit without continuation lowering
- `native_actor_task_await_capture_reject` — receiver/parameter capture
- `native_actor_task_await_nonsuspends_reject` — missing suspension effect
- `native_actor_task_await_wrong_owner_reject` — wrong Task identity
- `native_actor_task_await_missing_child_reject` — no typed child
- `native_actor_task_await_wrong_type_reject` — Boolean result where `Task<int>` requires i64

After an approved LLVM build, run:

```sh
ctest --test-dir build -R '^native_actor_task_await_' --output-on-failure
```

These are **compiler semantic negatives**, never a proof of source task execution. Existing synchronous actor guest tests and legacy top-level source Task<int> tests must remain passing.

## Implementation prerequisites before accepting a positive source guest

1. Generate an actor-owned heap/arena continuation frame, with explicit producer result, await destination, linear task ownership and cancellation/parent ancestry. Do not retain a C/LLVM stack pointer across suspension.
2. Replace the old synchronous source actor on_message ABI with a typed suspension outcome, while keeping a **logical nonreentrant handler lease** across await even when the physical carrier is released. Later DATA messages for that actor must remain pending until logical completion.
3. Reserve/transfer an exact-generation WAKE ticket while an authenticated actor DATA/TASK turn holds its physical lease, then publish readiness from external producers without executing guest code there.
4. Dispatch the child body on the same actor's native TASK lane and resume the suspended handler only from that actor's authenticated WAKE callback. Do not route either to ROOT_TASK, CONTROL, the synthetic SourceTaskRuntime pool, or foreign actor/domain.
5. Bind physical retirement to the trusted *after*-callback receipt, not merely `Future` settlement; STOP must cancel queued/suspended work and await all actor-owned child frames and external publisher loans before releasing the actor heap.
6. Add a compiled `.atto` positive with event ordering, parent/child cancellation, same-actor nonreentrancy, cross-actor concurrency, STOP-before-resume, stale generation and sanitizer tests; require independently reviewed exact compiler commit/tree, guest output and nonzero executed CI steps.

CI cannot be outsourced to arbitrary GitHub organizations just because an org has available minutes. Source policy in `.github/private-ci-approved-repositories.txt` and the PR-specific carrier contract still applies; #1732 tracks review of an authorized exact-tree alternate runner. No source export has been performed.

This document and source-gate branch deliberately keep the staged syntax visible for review **without claiming full AttoLang Task parity**.
