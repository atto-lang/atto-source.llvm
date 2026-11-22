# Atto diamond native LLVM source verification — 2026-11-15

**Status:** PASS for current **native class method lowering and explicit fail-closed
unsupported inheritance**, not a claim of virtual/trait/multiple-inheritance
native guest support.

## Source and carrier provenance
- Source: private `ores-truffle-oreslang/oreslang-source.llvm`
- Draft PR: [#1835](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1835)
- Source PR HEAD: `cca40c6df873e618da0066de4f83711f16f6e016`
- Complete upstream Git tree: `6b0a8da41b0569e9dd3f19b869a3ac3916475955`
- Scoped source subtree: `faf163477d013e1ca3155863dd355d3b225c352d` consisting
  of **319 exact Git blobs** with original SHA-1, file modes and paths:
  `CMakeLists.txt`, `src/main/**`, and `tests/native_class_methods.py`.
- Alternate carrier: private `zed-pkg-test/oreslang-private-pr-validation-20261012`,
  branch `validation/atto-llvm-diamond-20261115`.
- [Executed Actions run 43760108733](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/43760108733):
  **SUCCESS**, actual GitHub runner (not a zero-step admission failure).

The original source organization has runners unavailable on this PR
(`runner_id=0`, `steps=[]`). This was a **scoped exact-blob source mirror**,
not a complete all-files repository mirror or a public code export. The carrier
used its own Actions token to check out its own private branch; no cross-org
token/secret or artifact publication was required.

## Executed compiler-backed evidence
- Installed LLVM 19, Clang 19 (Ubuntu clang 19.1.1), Ninja/CMake;
  configured with `BUILD_TESTING=OFF` for the compiler-only selection.
- Compiled and linked actual `oreslang-llvmc`, SHA-256
  `4f09ce8dc6b396dcfe91be4da2e36fed463096c8f6a0b92a4996b5c055355a71`.
- Ran real `tests/native_class_methods.py`: **36 cases passed**, including
  actual Clang-compiled class guest binaries returning 42 and the new
  inheritance/interface diagnostic/no-IR negatives.
- Ran the same 36 cases under **Clang ASan + UBSan**: **36 passed**.
- Compiled a direct instance method guest with Clang `-O0` and `-O2`;
  both independent binaries executed with **exit code 42**.
- Exact website `playground-exact.atto` (Git blob
  `05814acb37c0335c03294875b870560a2839031d`) is **not supported**
  by this native LLVM frontend and must fail with **no emitted LLVM IR**:
  executed test **PASS**. Do not misinterpret this as source acceptance.
- Additional explicit class-extends probe rejected
  `NATIVE_CLASS_INHERITANCE_UNSUPPORTED`, output **zero LLVM IR**, test PASS.
- The test matrix's Java differential mode was **not enabled**; its actual
  log reported `Java differential=0` (no cross-backend executable comparison).

## Missing parity; no release claim
Native LLVM currently supports bounded scalar concrete classes and direct
class-method AOT, but does not yet lower class `extends`, multi-parent class
layout, virtual base-typed dispatch, class statics, or concrete trait methods.
Java JVM has separate executed virtual dispatch evidence but does not admit
the exact preview's trait defaults/explicit `override` syntax. Rust/Wasm
supports exact preview source and concrete static inheritance lowering, not
base-typed dynamic dispatch. These are distinct capabilities and **must not**
be combined into a false five-backend parity claim.

Next milestone needs a typed native parent layout and vtable/trait-witness ABI
with ownership, return/field/constructor safety, deterministic method slot
identity and compiled guest tests on the unchanged website sample under Clang
O0/O2, ASan/UBSan, plus Java/Rust differential observations. Until that exists,
fail closed without IR/binary emission.
