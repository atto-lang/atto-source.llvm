# `sync task` — initial executable Rust LLVM scalar profile

This draft implements **only** non-suspending, scalar inline task bodies in the
Rust `atto-llvm` source-to-LLVM-IR compiler and its shared checked frontend.
This is not a `task<T>` / `Future<T>` scheduler implementation and is not
certification for the separate C++ `oreslang-llvmc` path.

The parser recognizes `sync task<T> { ... }`, inferred `sync task { ... }`,
`sync task {}`, and `do sync task<T> { ... }`. A scalar result requires
a final direct `return expression;`; that return exits only the inline
task expression. The body may use scalar bindings, assignments, conditional
statements and nested sequential blocks, with their usual checked effects.
The value is available immediately after the body executes. `do` discards it.

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
