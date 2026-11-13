# LLVM native actor suspends Task<int> — typed AST / compiler safety boundary

Status: **typed source boundary implementation; no compiled actor task/await guest execution**. This branch is stacked on native actor TASK/WAKE PR #1734, but **does not** replace its runtime with a foreign Task executor.

## Canonical source slice

```atto
define actor Worker as {
  let total: int = 0;
  pub suspends fnc on_message(int incoming): int {
    let Task<int> child = task { return incoming + 1; };
    let int answer = await child;
    return answer;
  }
}
```

`pub suspends mtd on_message(...)` is also accepted by the restricted parser. This is a *shape/type admission*, not an executable source construct. Both forms must currently fail in LLVM emission with the explicit diagnostic:

`native actor Task<int> requires compiler-owned continuation frame and actor TASK/WAKE ABI lowering`

## AST and compile-time ownership proof

- `ActorSuspendingTaskI64` records exactly one named `Task<int>`, a pure scalar producer expression, the one copied incoming `int` capture, one consuming `await` owner, and an immutable result binding.
- The only allowed captured variable is the `on_message(int incoming)` parameter; accepted expression nodes are integer literals, that variable, unary negation, and bounded `+`, `-`, `*` trees. This prevents actor field/heap handles, `self`, FFI calls, borrows, closures, other tasks, and host pointers crossing an unimplemented suspend boundary.
- Task and awaited result must not shadow the parameter or actor fields, and the awaited name must be the exact lexical Task owner. The return must be the exactly awaited result.
- `suspends` cannot be combined with `critical` or another suspension modifier. A synchronous actor still rejects `await`.
- Semantic validation checks the captured i64 expression only against the copied parameter, then validates the result against the newly introduced lexical awaited-result name. It **does not** manufacture a corresponding LLVM stack slot or permit native emission.
- LLVM codegen checks `actor.suspending_task` before defining actor functions and refuses this staged AST. Legacy `main`-owned SourceTask I64 pool is never a legal execution fallback for an actor.

## Compiler test fixtures (all canonical .atto)

- `native-actor-task-i64-typed-gate.atto`: valid restricted `suspends fnc` AST, emission denied explicitly.
- `native-actor-task-i64-mtd-gate.atto`: identical admission through `suspends mtd`, emission denied explicitly.
- `native-actor-task-field-capture-reject.atto`: actor-state capture denied.
- `native-actor-task-ffi-capture-reject.atto`: untrusted call/FFI capture denied.
- `native-actor-task-wrong-await-reject.atto`: wrong Task owner denied.
- `native-actor-task-wrong-return-reject.atto`: incorrect return binding denied.
- `native-actor-task-mixed-effect-reject.atto`: mixed `critical` / `suspends` denied.

Run through the real `oreslang-llvmc` compiler using the registered CTest cases:
```sh
ctest --test-dir build -R '^native_actor_suspends_' --output-on-failure
```

**Evidence:** tests are authored and registered, but on this branch no new-head executed compiler/guest receipt has been obtained. A static code read or CTest registration is NOT a test PASS. Source repository Actions have had zero-step runner admission failures. The private CI mirror allowlist and PR-specific carrier authorization must be independently approved before any transfer.

## Required next implementation before positive execution

1. Make the compiler produce an actor-owned, bounded frame containing only the copied scalar capture, continuation PC, exact actor incarnation and generation-fenced Task identity; memory must live in the actor's heap/arena, not the DATA or producer stack.
2. Generate separate code for the initial actor DATA admission and the Task WAKE continuation; wire them through `NativeActorTaskRegistry` on the exact physical actor scheduler. Do not use `SourceTaskRuntime`'s trusted-main Task pool.
3. Register Future readiness atomically with suspension, including already-completed Futures and the early-result escrow; commit `SUSPENDING -> SUSPENDED` **only** in the trusted after-WAKE callback after the guest frame physically exits.
4. Ensure STOP recursively cancels child scopes and waits for physical quiescence, rejects late WAKE, and destroys frame owners exactly once after the scheduler retirement receipt. Include adversarial negative/mutation tests.
5. Run actual compiler-generated `.atto` guest executables with GCC/Clang under O0/O2, ASan/UBSan, compare against the Java reference, obtain independent review and merge.

Host-level bridge tests, AST admission, or syntactic fixtures alone must never count as positive AttoLang backend parity.
