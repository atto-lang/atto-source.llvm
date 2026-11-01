# Native LLVM ADT source-parity gate (2026-11-01)

LLVM's source parser deliberately refuses `define enum` except the
separately supported internal `MaxActorThreads` special case. It has host
enum overload policy and some tagged-wire models, but **not** production
source-level nominal constructor/typed payload matching semantics.

The new CTest cases `native_adt_unsupported_*` invoke the actual
`oreslang-llvmc` on six **byte-identical** Oreslang fixtures from the
canonical cross-backend suite in `oreslang-platform-conformance` draft #270.
They require exit 1, the exact **unsupported feature diagnostic**, and
no emitted IR stdout. They prove only that unsupported source is refused;
none may count as a semantic negative or positive compiler pass.

| Fixture | Canonical Git blob SHA1 |
| --- | --- |
| nullary.ores | a59622d03e0c2d66a28c08d082140b769f747499 |
| payload.ores | ece5ad07cfc0802907f0fc18a306b8f726d3f169 |
| owner.ores | 8ea4459d0e89adce0236fef0daa29833bb5a2e87 |
| max-tag.ores | b0d58c31c25afd871f739ed8538f0e17196aaa3e |
| guarded-not-exhaustive.ores | 80795a49084538e1c4e3ba79197edcff92ddf2aa |
| nested-foreign-payload.ores | 53755c22c73bd7e8d3ad1ecade60e8c6acabbeea |

To close parity, implement source enum AST/type/ownership resolution and
nominal qualified owner identity, finite product/sum exhaustiveness,
checked layout and constructor lowering, then change the six **accept**
or **semantic-reject** expectations to true verified compiler diagnostics,
native IR/Clang execution and ASan/UBSan evidence. Promote only after all
36 canonical cases pass on the same exact-source revision and
independent CI review; a zero-step workflow counts as runner admission
failure, never a test outcome. The CTest reservation and host wire models
do not certify .NET or Wasm runtime parity.
