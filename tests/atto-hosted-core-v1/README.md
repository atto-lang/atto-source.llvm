# LLVM hosted canonical core.atto — executable parity candidate

This draft adds the byte-identical AttoLang guest algorithm from Java/GraalVM
`stdlib/core.atto` into LLVM's **closed compiler-owned hosted registry**.

- Original canonical Git source blob: `d2cd9b8e0cd3e3bb3ee6a5f9cebf9ab3312c44f4`.
- Existing JVM-linked module identity: `@std/core.ores`.
- Supported spelling candidates: `ores.std/core`, `ores.std/core.atto`,
  `ores.std/core.ores`, `std/core`, `std/core.atto`, `std/core.ores`.
- No `ores.std/assert.atto` admission, `java:` imports, ambient project
  source fallback, unexpected `.ores` aliases, or freestanding target access.

`bash tests/atto_hosted_core_parity.sh --source-only` checks the canonical
source Git blob and exact bytes embedded in the compiler; it NEVER certifies
native execution.

`bash tests/atto_hosted_core_parity.sh --run` requires the real LLVM compiler,
real IR, no-GC IR auditor, Clang O0/O2, six native exits of 42, three negative
compiler rejections with no partial IR, and a hosted-only freestanding rejection.
Host-observed receipts are emitted only after successful guest execution.

**Status: AUTHORED, NOT_EXECUTED.** GitHub's source-org Actions currently fails
before runner admission with `steps=[]`. No LLVM performance, codegen, Native
Image, .NET, Wasm, or Forge parity pass is asserted; independent review and
exact-head executed receipts are required before merge. No private source mirror
or cross-organization transfer is used.
