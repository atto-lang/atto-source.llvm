# Native actor-send ownership admission

Current native source actors send **i64 value payloads**, not guest pointers,
loans, non-Copy class objects, continuations or GC-managed references.

`OwnershipChecker` validates every `ActorAction::Send` payload, in the
identical source order used by `LlvmCodegen`, after local bindings and
ordinary statements and before return-edge analysis. Valid scalar `rt take`
transfers invalidate the sender's local value; a later send/return or
expression read of that value is a compile-time error with **no emitted IR**.
Call-scoped borrows may be used inside a function that returns a plain i64
value before sending. Direct `rt borrow` as a send payload is forbidden.
The parser separately validates actor reference identity and send/stop order.

Checks do **not** enable class-valued actor messages, actor heap migrations,
`await`, continuation suspension, cancellation or cross-actor loan transfer.
Those require typed payload layouts, provenance and quiescent cleanup.
The current bounded primitive-field subset permits `rt take obj.field` for
trivially destructible i64 fields; the checker invalidates that field until
explicit reassignment. A second send reading that moved field must fail.
Nontrivial class fields, actor object transfers and owned-reference payloads
remain unsupported. Experimental region-owned `int` transfers are gated by
the separately audited runtime live flags.

Executable regression coverage lives in `tests/source_actor_e2e.py`:
valid moved scalars and reborrow-then-move go through real LLVM and native
Clang with the existing actor heap/scheduler, including sanitizer builds.
Invalid borrowed payloads, use-after-move in another send or a return,
use-after-move within a single expression, and class escape/field moves
must reject without any emitted IR.

See [ownership parity tracker](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/96).
