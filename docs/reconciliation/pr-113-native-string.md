# PR #113 — native String ABI conceptual conflict resolution

Historical branch `0fd619b0314f1f4524a57e59fb69404d486204dd`; current LLVM main `b4db9428802fe98153780b403a55059817722eef`.

The current compiler and type-only conformance gate fail closed for String client contracts and incomplete/unsupported callable source ABI, as required by newer #73 contracts. The historical entire `Parser.hpp`, `LlvmCodegen`, CLI and experimental String frontend cannot replace the current actor/task/effect/ownership compiler, and a parsed signature is not an executable native ABI promise.

Forward-port the self-contained `NativeString.c/.h` host UTF-8/string-ownership and bounded JSON escaping/unescaping library plus `native_string_test.c`. Add a standalone C11 CMake/CTest target with GCC/Clang -Werror. The host C test checks ownership, UTF-8, malformed/lone-surrogate JSON escaping, invalid lengths and output-buffer boundaries. This library is not linked to the guest LLVM code generator or exposed to Oreslang source until a future audited owned-string ABI/generation-checked allocator gate passes.

**Not delivered**: `--check` String-client signatures, native String guest lowering, cross-language SDK interop, and type-level ownership of the proposed two-field owning C struct. Those remain in historical first-parent commits only. Require exact-head compiled CTest plus ASan/UBSan, 32/64-bit ABI checks, full source parser negative gates, and independent approval before merge.
