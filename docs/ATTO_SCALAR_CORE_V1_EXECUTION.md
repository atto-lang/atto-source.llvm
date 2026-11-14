# LLVM AttoLang scalar-core v1 execution

This branch imports the exact 19-file `attolang.scalar-core.v1` corpus from
`oreslang-common-source`. Each source file retains the same Git blob SHA-1.

The execution driver builds each case with the real `oreslang-llvmc`. For
positive programs it verifies the generated native IR exports
`__oreslang_guest_main(): i64`, renames only the generated OS `i32 main`
wrapper in a temporary copy, links a fixed C observer, and calls the full-width
guest entry. This is necessary because Unix process exit status cannot represent
the signed i64 boundary cases.

Negative cases fail closed:

- immutable rebinding and removed `%`, `^`, `^^` operators must reject
  before any LLVM IR is published;
- checked add/sub/mul overflow and eager unused overflow may reject during
  compilation or compile and terminate through the checked runtime trap;
- a normal wrapped value is always a failure.

The driver emits a candidate `attolang.parity-receipt/2` with exact source
head/tree, compiler digest, per-case source SHA-256, per-compiled-case observer
artifact digest, compiler-IR digest, result channel, and compile/runtime failure
stage. It is not self-authenticating and is never sufficient by itself for
public parity promotion; GitHub run identity and nonempty executed steps must be
verified independently.

The parser change also aligns LLVM source admission with the merged scalar-core
contract by rejecting legacy `%`, `^`, and `^^` spellings. Internal enum
kinds remain for compatibility with older generated/internal structures; source
spelling is what is removed here.
