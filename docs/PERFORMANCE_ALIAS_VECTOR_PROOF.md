# Alias analysis, vectorization and ownership proof

Status: **draft contract**. TBAA and `noalias` metadata are optimizations only when semantics prove they are true; assigning different metadata based on nominal types alone is unsound.

## Allowed proof sources
- Two simultaneously live exclusive owned regions with distinct allocations and disjoint storage, including all reborrows.
- An explicit nonoverlap proof for subranges of the same buffer, including bounds and stride.
- An independent immutable read borrow does **not** prove absence of other reads. A mutable loan is exclusive for its checked lifetime only.
- FFI, device views, shared regions, pointer casts, suspension and untrusted callbacks invalidate claims unless modeled explicitly.

## Backend experiment
1. Select the already supported source-neutral typed DOP F32 CPU kernel, not arbitrary guest `simd` syntax.
2. Run LLVM verifier and strict alias/provenance tests before adding scoped `alias.scope`/`noalias`/TBAA metadata.
3. Benchmark and inspect LLVM loop optimization remarks on x86-64 and AArch64, with feature flags pinned and scalar fallback controlled.
4. Evaluate misalignment, partial vectors/tails, zero/one element, overlapping slices, large strides and read/write conflicts.
5. For floats, retain the declared contract: no unapproved reassociation/fast-math, NaNs, signed zero and rounding handled exactly; for integers test overflow/truncation.

## Adversarial regression matrix
- [ ] Same-buffer alias, partially overlapped buffer, equal pointer with distinct nominal types
- [ ] Two different allocations and provably disjoint subranges
- [ ] Stale borrow, moved owner, drop, actor transfer and suspend
- [ ] FFI pointer escape, unknown external call and mutable shared access
- [ ] Scalar reference and SIMD implementation agree on admitted inputs

## Required evidence
Actual compiler remarks and disassembly showing emitted SIMD lanes, + pinned workload/CPU/toolchain and repeated measurements. If vectorizer declines, mark `NOT_VECTORIZED`; never claim success based solely on an enabled flag.

Tracking: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/579
