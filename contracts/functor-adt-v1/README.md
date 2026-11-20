# AttoLang LLVM nested functor conformance: test-only draft

## Scope and exact upstream source

This directory contains 17 byte-for-byte identical .atto fixtures and an exact
versioned manifest from the platform-conformance PR #689. They originate at
Java source commit faa149c6bcc7510a659c86499f614efdaffcc4e8; fixture Git
tree be201b1c667057582a6e7e53025bb1d1938179c6.

The two positives contain 36 Java/GraalVM-verified observable guest assertions.
The 15 negatives have been rejected by Java. This does NOT imply LLVM support.

## Added LLVM test families

- Git SHA1 on 17 source blobs; exact immutable subtree, source head and per-backend pending state
- Functor identity and composition over 8 numbers, 5 errors and Some/None, Ok/Err variants
- Two transpose directions and round trips over successes, absences and error states
- Triple-nested map, single-layer flatten, associativity and error-tag preservation
- Lazy callbacks: None/Err cannot trigger map, and Ok must bypass map_err
- Four mutation controls for unsafe collapse, Err loss, eager callback and changing Ok
- Real oreslang-llvmc admission of all 17 byte-identical .atto sources
- Actual Clang O0/O2 native executable and exact output checks for ANY positive LLVM case admitted
- Rejection of invalid sources that accidentally compile; no partial IR on compiler errors

**Evidence tiers are deliberately separate:** the Node law suite is source-free
and cannot count as native LLVM execution. A parse or unsupported-feature
diagnostic on a negative is NOT semantic type or ownership proof. The native
admission runner records such cases without claiming parity, and cannot
promote any LLVM backend status automatically.

Still pending: exact diagnostic-category checks on LLVM; real native positive
functor executions; affine owner/drop counts; ASan/UBSan; full native zero-GC
allocation/lifetime proof; other-backend guest parity.

Run the source-free checks:
    node --test tests/functor_adt_v1_integrity.test.mjs

For real compiler probes, build oreslang-llvmc from this exact source head,
compile scripts/audit_no_gc_ir.rs with Rust, then run:
    ORESLANG_LLVMC="$PWD/build/oreslang-llvmc" \
    ORES_NO_GC_IR_AUDIT="$PWD/build/oreslang-no-gc-ir-audit" \
    node tests/functor_adt_v1_native_admission.mjs

No production LLVM compiler, actor heap or ABI is changed by this draft.
