# Browser native C ABI: exact `uint32_t` version export, P0

**Status:** source/compiler implementation proposed; real native compiler execution
must complete in CI. Not a functioning browser backend, not actor-state lowering,
not Qt integration, and not a general foreign-function interface.

## Source declaration

```oreslang
@CExportU32("ores_browser_abi_version")
pub fnc browser_native_abi_v1(): int {
  return 1;
}
```

The compiler emits TWO independent functions:

- the internal-linkage Oreslang guest `i64 @browser_native_abi_v1()`,
  using the current native guest `int` convention, never exposed through
  the ELF dynamic symbol table;
- a generated `i32 @ores_browser_abi_version()` external C function,
  which calls that guest function, independently checks the unsigned
  `uint32_t` range and truncates only after the check.

The native C consumer uses `uint32_t (*)(void)`. It must never cast the
guest `i64` function pointer to a C `uint32_t` function pointer.

For this *first audited slice*, the compiler permits **only the exact symbol**
`ores_browser_abi_version` and a public, synchronous, zero-argument,
non-generic function returning the **literal integer 1**. No aliases,
modules, namespaces, actor methods, effects, hidden parameters, arrays,
borrowed pointers, String/sums, nonliteral results or alternate exported
names are permitted. Annotation arguments must be literal strings.
Symbol collision is rejected (also in LLVM codegen, including later
declarations). Unsupported forms fail before emitting LLVM IR.

This intentionally narrow admission establishes an actual compiler-generated
ABI boundary, not a C/C++ implementation of browser policy. Expanding to
arbitrary C symbols, pointer/struct parameters, 64-bit handles, allocator
lifetimes and error unions requires a separate language/backend contract and
conformance tests on Java/GraalVM, LLVM, Wasm and .NET.

## Actual guest-to-host acceptance

`scripts/verify-browser-c-abi-u32.sh` is run against a clean exact Git
checkout and an identified native `oreslang-llvmc` binary:

1. Require checkout SHA and compiler SHA256 matches, compiler native ELF,
   and a clean working tree.
2. Compile the positive source with **real** `oreslang-llvmc` to verified
   LLVM IR. Assert exactly one exported i32 C symbol and one **internal** i64
   guest definition.
3. Clang links the generated IR into an ELF shared library with eager,
   defined-symbol, RELRO and NOW linker options.
4. Inspect actual dynamic symbols, then execute a strict C11 `dlopen` host
   that calls **only the i32 C entrypoint**. It confirms the private i64 guest
   is not accessible through `dlsym` and refuses an accidentally present
   open/dispatch/snapshot/close export.
5. Compile eight negative fixtures. All must fail and emit **zero LLVM IR**:
   wrong symbol, private function, out-of-range literal, parameter,
   wrong return type, colliding unannotated symbol, nonliteral/effectful
   body, duplicate annotations.

This is a **real compiler acceptance** only after all five steps are
observed executing on a runner; static code inspection, synthetic LLVM IR,
and `runner_id=0`/zero-step jobs do not establish execution.

## Next implementation dependency

`ores_browser_open`, `ores_browser_dispatch`, `ores_browser_snapshot`,
`ores_browser_close` require an actor-owned state/FFI session model,
opaque handles and exact-sized C structs. No implementation of those symbols
is included here. See
[LLVM browser issue #1163](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1163)
and [browser issue #6](https://github.com/ores-truffle-oreslang/oreslang-web-browser.ores/issues/6).

## Cross-organization CI

A source-free canary in the private `usa-acc/usa-acc-e2e` repository has
executed Clang, synthetic LLVM IR shared-library linking and C `dlopen`.
**No private LLVM source may be mirrored** until an independent authorized
reviewer explicitly approves the exact immutable Git tree and destination.
Approval [issue #1172](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1172)
remains a gate. It must be updated for this new branch/tree; the approval
request for PR #1167 does not automatically authorize this branch.
