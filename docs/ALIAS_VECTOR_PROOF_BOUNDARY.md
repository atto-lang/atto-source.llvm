# LLVM alias and vectorization proof boundary — draft

Tracks [#579](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/579)
and [cross-target optimization epic](https://github.com/ores-truffle-oreslang/.github/issues/5).

Before adding LLVM TBAA, `noalias`, `alias.scope`, forced loop vectorization
hints or fast-math, the Oreslang ownership/borrow/effect verifier must prove that
the corresponding transformation preserves all guest semantics. **Nominally
distinct types do not imply distinct memory locations.** In particular, actor
arenas, shared frozen views, overlapping slices, FFI pointers, suspended borrows
and mutable state loans need precise lifetime/alias analysis.

```sh
python3 -m unittest discover -s tests -p 'test_ownership_opt_ir.py'
python3 scripts/verify_ownership_optimization_ir.py /path/to/generated.ll
```

The tool rejects unproven optimization **annotations** in a bounded textual IR
file. A passing result merely reports `UNPROVEN_OPTIMIZATIONS_ABSENT`, not
successful vectorization, scalar replacement, ownership correctness or improved
runtime. It is currently an opt-in audit, **not compiler emission integrated**.
LLVM may still auto-vectorize unannotated eligible loops, requiring independent
opt remarks, disassembly and exact correctness tests.

Before this becomes a compiler gate:
1. Pin source-to-IR and ownership proof artifacts to exact source tree SHA.
2. Design a typed/region/slice provenance witness and verified metadata builder.
3. Independently test overlap/borrow/drops, sanitizer coverage, and
   integer/floating-point edge cases; fast-math needs explicit opt-in.
4. Inspect LLVM vectorization remarks and actual x86-64/AArch64/Wasm SIMD
   opcodes, benchmark scalar and SIMD on held-out workloads, and report target
   unsupported cases rather than an assumed speedup.
