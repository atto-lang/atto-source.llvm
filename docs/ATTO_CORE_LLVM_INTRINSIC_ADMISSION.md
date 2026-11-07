# Core freestanding LLVM intrinsic admission hardening

The IR audit no longer ignores every `llvm.*` external declaration. Only
known exact and typed LLVM intrinsic families are exempt; unknown names
(including forged `llvm.ores_actor_send`), unsupported debug intrinsics,
missing type suffixes and quoted external names are reported as imports.
Undefined compiled-object symbols are also checked against the separate
exact 27-symbol freestanding SUPPORT_ABI allowlist. New Rust tests exercise
five adversarial groups.

This is defensive source-level admission. No compiler, linker or QEMU
execution may be claimed until GitHub supplies a real runner and immutable
head-bound receipts. New intrinsic families require explicit review before
being admitted.

## Typed-suffix anti-spoofing (Core v1)

The second audit pass removes an additional forged-intrinsic exception:
typed families such as `llvm.memcpy.` now only exempt scalar integer
(`i32`, `i64`) or opaque pointer (`p0`, `p1`) type tokens, optionally
dot-separated. Arbitrary text like `llvm.memcpy.host_escape`,
`llvm.smul.with.overflow.i64.backdoor`, or `llvm.ctpop.i64_payload`
is classified as an external import and rejected. Scalar and pointer
positive controls guard the actual allowed ABI. Vector/non-scalar or
future LLVM typed families require explicit compiler review; this
gate cannot substitute for the separate compiled-object and ELF checks.
