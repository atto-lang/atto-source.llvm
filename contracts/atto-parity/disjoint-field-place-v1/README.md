# Disjoint scalar field-place loans — cross-backend compiler corpus (draft)

This is a **staged, review-only LLVM parity corpus**, not a claim that the native LLVM compiler implements or has executed these source cases.

Canonical source: [atto-source.rs #477](https://github.com/atto-lang/atto-source.rs/pull/477) at exact head `4a623a2b41dc29d08ab15753011f6cfb85f6106e`; stacked after Rust #476. The 16 source fixtures here preserve the Rust PR's source text (10 denial, 6 positive) and the inline declared expectations. The accompanying manifest pins source identity and expected outcomes.

## Intended LLVM semantic contract

- A compiler-issued `borrow! mut person.age` denotes an authenticated mutable **field place**, not a detached copy. Writes through the alias change the owning storage.
- Distinct, known scalar fields can have simultaneous exclusive loans. Overlap of a shared/exclusive loan on one field, owner moves/reads/writes that conflict with an active field loan, immutable root/field and nested/unproven projections must be rejected.
- Never infer permission to borrow a whole object, actor-private state, method effects, foreign pointers or cross-TenantVM memory from these scalar examples.
- The Rust implementation intentionally does **not** prove LLVM source lowering. LLVM #678 concerns lower-level lexical-loan/ticket identity, not these end-to-end source cases.

## Promotion gates

1. Independently reconcile the exact Atto syntax with current atto-spec and formatter; `// java: rejects` is a source comment, not an assertion about Java's present head.
2. Implement native parser, semantic field-place identity and checked lowering (or an explicit per-case unsupported diagnostic), without emitted IR for rejected cases.
3. Compile all six positive sources at O0/O2 into real LLVM IR, then Clang native executables; verify exact return 42 and stdout. Reject all ten negatives with documented codes and no IR/artifacts.
4. Verify sanitizer runs and exact source-head/compiler-artifact provenance using a fully authorized CI runner with **nonzero executed steps**. Do not export private repository trees without independent source policy approval.

**Current evidence: NOT_EXECUTED in LLVM. Do not merge or advertise parity yet.**
