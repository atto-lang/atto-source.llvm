# Native LLVM optional-type and omittable-formal admission — 2026-11-14

**Status: executable compiler rejection gate authored, not stepfully verified.**

AttoLang distinguishes `T? name` (a **required** `Option<T>` value) from
`T name?` (an **omittable formal**). `T? name?` combines them, and a default
without `name?` is malformed. Native LLVM's scalar compiler currently
implements neither tagged Option payload call/return ABI nor omission-aware
parameter binding. Therefore all of those forms must fail *before emitting
LLVM IR*, with separate compiler-owned diagnostics rather than silently
mapping both question marks into null or a lower source arity.

The new `native_optional_named_parameter_presence_fail_closed` CTest calls
the real LLVM source compiler on canonical `.atto` files (two accepted native
scalar positional/named controls, nine rejected Option/omission/default
sources). Accepted controls must emit IR, link with Clang and exit 42.
Rejected sources must have return code 1, empty stdout (no IR) and the
expected diagnostic prefix.

This is **not an Option runtime implementation** and does not establish
Java/Graal/.NET parity. It is a conservative source admission milestone that
keeps future optional argument lowering from becoming an accidental dynamic
label/ABI path. No JIT, Native Image, sanitizer or hard-RT evidence is claimed.

Canonical contract: https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/51
Java main stage: https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/3078
.NET closed int? local stage: https://github.com/ores-truffle-oreslang/oreslang-source.net/pull/520
