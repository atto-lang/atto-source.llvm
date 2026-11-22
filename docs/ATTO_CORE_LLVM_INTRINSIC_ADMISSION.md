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

## IR whitespace and malformed-declaration denial

The import auditor now recognizes `declare\t` and other whitespace
following LLVM's `declare` keyword; it no longer relies on exactly one
ASCII space. It also refuses to silently drop a declaration missing a
parseable `@name`: such lines become a nonempty unparseable-external
diagnostic and cannot certify a freestanding image. Three additional
Rust regression groups cover tabs, repeated spaces, malformed import
rows and quoted intrinsic spoofing. This is a compiler IR safety check;
object and ELF audits remain mandatory, and hosted execution evidence is
still blocked by GitHub runner admission.

## LLVM declaration lexical isolation

The freestanding auditor now locates `@symbol` only **outside** quoted
IR metadata and before a real unquoted `;` comment delimiter.
A malformed `declare` statement followed by `; @llvm.trap`
previously risked being interpreted as a legitimate intrinsic rather
than an unparseable external import. Three Rust regression cases
cover comment-only intrinsic deception, a quoted attribute containing
`@llvm.trap`, and semicolons in quoted external symbol names. The
object-level undefined symbol and fully linked ELF admission checks
remain mandatory. These new Rust cases are not yet hosted-executed.
