# `sync task` — initial executable Rust LLVM scalar profile

This draft implements **only** non-suspending, scalar inline task bodies in the
Rust `atto-llvm` source-to-LLVM-IR compiler and its shared checked frontend.
This is not a `task<T>` / `Future<T>` scheduler implementation and is not
certification for the separate C++ `oreslang-llvmc` path.

The parser recognizes `sync task<T> { ... }`, inferred `sync task { ... }`,
`sync task {}`, and **void-only** `do sync task<void> { ... }` or
`do sync task { ... }`. A scalar result requires a final direct
`return expression;`; that return exits only the inline task expression.
The body may use scalar bindings, assignments, conditional statements and
nested sequential blocks, with their usual checked effects.

The `do` form is not a value-discard operator. Only `void` is permitted:
`do sync task<int> { ... }` and `do sync task<bool> { ... }` are
`E_TASK_RETURN_TYPE` even if the body is empty. Inferred `do sync task`
also rejects `return expression;`, as does `do sync task<void>`.
An explicitly nonvoid `do` payload is diagnosed before its body's unsupported
effects, so `do sync task<int> { return await x; }` cannot evade the
void-only contract through a different diagnostic. A final bare
`return;` is legal and exits only the inline lexical body.
To produce a value, use `sync task<int> { return 42; }` without `do`.

The checked IR retains an explicit `InlineTask` node; the LLVM emitter emits
`InlineTaskEnter` / `InlineTaskExit` markers and executes the body at the
call site on the same task, with no Future, child TaskId or task dispatch.
The guest Wasm frontend follows the same no-spawn ordering.

**Fail-closed restrictions:** the current profile admits only int, bool and
void values and AST-proven scalar expressions. Calls, suspension, yield,
guard acquisition, deferred cleanup, throws, early/nested body returns,
spawned tasks, independently scheduled `task<T>`, and `await task<T>`
are not admitted. These require transitive effect checking, safe continuation
frames, structured cancellation, exact-once LIFO cleanup, and TenantVM scope
accounting. Neither `sync` nor this profile permits blocking an OS worker.
Unknown input must produce a diagnostic and zero native IR bytes.

Canonical target: atto-spec #129, including the 2026-11-20 same-logical-TaskId
revision. Existing atto-source.llvm #72 is upgraded from reservation-only to
this deliberately narrow executable slice. No full backend parity is claimed.

## Separate native C++ path

The canonical C++ `oreslang-llvmc` parser now reserves `sync` and explicitly
returns `E_UNSUPPORTED_SYNC_TASK` before any LLVM IR emission. It does **not**
claim executable inline-task parity with the Rust `atto-llvm` path.
`tests/sync_task_native_cpp_gate.py` checks exact-token rejection (including
empty and `do` syntax) and makes sure comments and `synchronize` do not trigger it.
