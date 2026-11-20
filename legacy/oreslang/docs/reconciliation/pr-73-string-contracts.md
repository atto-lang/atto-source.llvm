# LLVM PR #73 — String client contract semantic reconciliation

Historical head `326869a4af1e1e35675890b81a2f6ac4777f575e`; LLVM main `bd65b56ca32300a0d161323472807e71a61b3219`.

LLVM main already has *native int-only* `define contract` declarations and type-only import/ABI validation. In `Parser::contract()`, parameter and result spellings require `int`; it explicitly rejects unsupported signatures. Do **not** restore the historical standalone String-contract `Parser.hpp`, legacy `--check` flag or type-only fake LLVM runtime lowering over modern parser/main/ABI code. A declaration-only String client contract may not emit IR until its actual typed wire ABI is implemented.

Retain the two historically pinned public String wire-contract fixtures verbatim. Add exact diagnostic **negative** CMake/CTest checks using the real compiler and a Python-free CMake harness. These tests assert that unsupported String contract surface fails for type-relevant reasons, not accidental CLI failures. No runtime contract conformance or interop is asserted.

**Deferred feature:** backend parity for String signatures, real imported contract linkage, --check no-IR contract mode and wire validation. Review before enabling any such source; exact-head CMake/CTest and LLVM/Java conformance are still required. Historical legacy positive claims are not accepted as current release evidence.
