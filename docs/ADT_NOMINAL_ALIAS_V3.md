# Native LLVM closed-enum match aliases V3 — restricted AOT implementation

**Status: DRAFT / ACTUAL NATIVE EXECUTION NOT YET VERIFIED on this PR head.**
**Spec:** `attolang-language-spec.md#60`.
**Stacked on:** LLVM #1683 (itself stacked on #1679).

This change extends the existing restricted opt-in path
`oreslang-llvmc --experimental-nominal-enum-v1 file.atto`.
It is an actual compiler frontend translator into the checked scalar compilation
path, **not** general tagged enum IR. The emitted LLVM IR is produced by
`oreslang-llvmc`, not a host-only enum evaluator.

### Admitted source subset

```atto
define enum Light {
    Red,
    Green,
    Yellow
}
fnc next(Light state): Light {
    do match state over
        on Red as original -> { return original; }
        on Green as g -> { return Light.Yellow; }
        on Yellow -> { return Light.Red; }
    end
}
pub fnc main(): int {
    val Light active = next(Light.Red);
    do match active over
        on Red as r -> { return 11; }
        on Green as g -> { return 22; }
        on Yellow -> { return 33; }
    end
}
```

- New canonical braces/comma form coexists with legacy `define enum Light as ... end`. Deterministic tags are private compiler representation, **never** a user-injectable integer enum ABI.
- Closed enum variants can be referenced without an owner qualifier in matches because the scrutinee type is statically fixed. Fully qualified variants remain accepted; foreign owners, unknown or repeated cases are checked.
- `as r` is an arm-local alias for an immutable **nullary Copy case**. The only allowed alias use here is `return r` from a private enum-typed helper; it lowers to the checked static tag. The guest never obtains an untyped integer or a mutable pointer.
- `do match` with terminal explicit `return` is supported in the restricted source lowering. Arbitrary statements or general expression `match` are **NOT IMPLEMENTED** by this change.

### Fail-closed limits

No payload-bearing enum, named fields, actor declarations or `mtd receive`, messages, actor send, cross-isolate transfer, deserialization, dynamic imports or public nominal function ABI are admitted in the opt-in path. No runtime tracing GC, reflection, per-arm heap objects or unverified numerical guest tag API is introduced.

The frontend rejects payload-bearing enum declarations and named payload field patterns with explicit fail-closed diagnostics, rather than generic punctuation errors. The C++ host-source admission suite `native_nominal_enum_alias_v3_source_admission` tests the real frontend lowering and 15 invalid source mutations. The source fixture `tests/fixtures/adt-nominal-alias-v3.atto` and workflow `.github/workflows/llvm-nominal-enum-alias-v3.yml` require an actual native LLVM19+-capable compiler build, emitted IR and Clang O0/O2 binaries exiting with code **11**. The workflow requires exact head SHA and source tree, read-only checkout, and rejects unsupported payload source without generated IR.

**Evidence status:** Checks are **AUTHORED NOT EXECUTED** at the time this draft was created. A source-only fixture and host test are not native guest proof. A zero-step GitHub Actions failure is **NOT_EXECUTED**. Private-source export to an alternate org cannot be justified only by runner minutes; complete independently approved exact snapshot/destination and protected-main policy gates first.

Before merging, reconcile this stacked branch against current main, independently audit case-pattern lifetime and AOT boundary security, run exact-head tests with stepful receipts, and verify the generated binary and negative error text. Java/Graal parity and future WASM/.NET/Forge work are separate acceptance gates.
