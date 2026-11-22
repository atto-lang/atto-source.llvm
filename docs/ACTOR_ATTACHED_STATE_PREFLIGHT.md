# Actor-attached protected state: LLVM parser/IR admission v1

**Status: parse-only and fail-closed native AOT; NOT executable Counter guest parity.**

This implementation consumes the canonical [atto-spec actor state contract #127](https://github.com/atto-lang/atto-spec/pull/127) and the exact portable source fixtures in [conformance #113](https://github.com/atto-lang/atto-platform-conformance/pull/113).

The source-local Rust frontend `target_syntax::parse_target_declarations` accepts
`define state on actor Counter as { let int count; }` as a distinct checked
`ActorStateAttachment`, retaining exact owner, name, type, initializer presence
and writable binding. It validates a matching nominal actor forward or local
concrete definition, duplicate state, missing/invalid field initializers, and
collisions with ordinary actor fields or methods. This does **not** construct
state memory, a `critical-write` capability, an actor incarnation, or a
supervisor control path.

`frontend::compile()` and the native `atto-llvm` `compile_ir()` reject
a valid attached state schema before emitting Wasm/native LLVM IR with an
`E_UNSUPPORTED` diagnostic. An invalid schema receives its checked diagnostic.
The exact user-supplied Counter forms are copied from conformance #113 as
identical Git blobs, and a negative native test requires them to stay
non-executable until full `on_start`, mailbox, `using state.read/write`,
guard and TenantVM lowering exists.

Unlike the JS trusted-worker preview, this LLVM change does **not** compile
or execute the Counter actor. The existing LLVM actor IR path remains
restricted and separate. Avoid porting the older Oreslang LLVM compiler's
state hooks without re-auditing ownership and incarnation authorization.

Validation requirements before Ready:
`cargo test --locked -p atto-compiler-browser`,
`cargo test --locked`, negative `atto-llvm` no-output compile checks,
Clang ASan/UBSan actor storage tests and exact source SHA, nonempty executed
runner steps. A zero-step CI failure is `NOT_EXECUTED`, never a compiler verdict.
