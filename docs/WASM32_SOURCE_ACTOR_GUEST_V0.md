# Compiler-generated single-actor Wasm32 guest v0

This narrowly scoped bridge compiles **actual Oreslang `define actor` source** into
import-free wasm32 LLVM IR. Unlike prior transport/C-runtime smoke tests, the
actor's own initializer, scalar field assignments and synchronous
`on_message(int): int` logic are lowered by `oreslang-llvmc`.

## Guest exports and lifecycle

Each WebAssembly.Instance represents **exactly one** actor, whose 1–16
integer fields occupy instance-private mutable Wasm globals. The compiler
generates:

- `oreslang_wasm_actor_init() -> void`, permitted only in NEW state
- `oreslang_wasm_actor_on_message(i64) -> i64`, permitted only in ACTIVE
- `oreslang_wasm_actor_teardown() -> void`, permitted only in ACTIVE

The lifecycle transitions NEW -> ACTIVE -> STOPPED, never backward.
Out-of-state direct guest ABI calls trap using Wasm `unreachable`.
Teardown overwrites every actor-owned field with zero and seals the instance.
No automatic initialization or source-defined `tear_down()` hook is implied.
A host must instantiate one module per actor, initialize once, serialize
each message on that instance's dedicated Worker, and call teardown once.
One Worker/instance ownership is a **host obligation**, not yet a formally
verified runtime scheduler property.

The generated guest has no host imports or native OresVM heap ABI. The
initial implementation intentionally supports one synchronous actor per
module with up to 16 scalar fields, simple field writes, a single i64
message argument, and an i64 result. The normal scalar/class Wasm target
and native OresVM actor lowering remain unchanged. Source `spawn`,
`send`, `stop`, suspension, multiple actors, native heap ownership,
ActorGroups, callbacks and supervisor CONTROL are still rejected on wasm32;
they are **not silently converted into sequential native semantics**.

## Executable test and security boundary

CTest `wasm32_source_actor_guest_lifecycle` invokes the current
`oreslang-llvmc --target=wasm32` on
`tests/fixtures/wasm32-single-source-actor.ores`, links its emitted IR
with real Clang/LLD, rejects any host imports, then executes 100 messages
inside Node Wasm. It checks independent instance state, no use before INIT,
no duplicate INIT, no post-teardown execution, no double teardown, and
two native-scheduler-dependent source fixtures that must fail without IR.

This **does not yet** implement the browser Worker host bridge, canonical
MessagePack ActorRef ABI, panic propagation, suspension/reentrancy,
supervised teardown, or full Java/native/wasm behavior equivalence.
The next milestone is to drive these exports from the existing bounded
Browser transport and prove observable equivalence for the same source
across all three backends.
