# LLVM sealed-scalar-v1 AOT admission (experimental)

The first strict source-level profile is deliberately **scalar-only**. It is
not a claim that all AttoLang guest features support sealed full AOT or that a
linked native executable has no collector in every runtime dependency.

Compile:

```sh
./build/oreslang-llvmc --aot-profile=sealed-scalar-v1 program.atto > program.ll
ORESLANG_LLVM_AOT_PROFILE=sealed-scalar-v1 \
  ORESLANG_LLVMC=./build/oreslang-llvmc \
  ORES_NO_GC_IR_AUDIT=./build/oreslang-no-gc-ir-audit \
  scripts/compile-aot.sh program.atto ./program
ctest --test-dir build -R native_sealed_scalar_aot_v1 --output-on-failure
```

The profile runs on a **fully resolved source AST**, before LLVM IR emission.
It checks the entire function graph (including non-exported functions) and
requires: a unique function name/arity identity, a known `main/0` entry,
known direct callee edges, bounded graph size and a modeled pure scalar
effect set. It rejects unverified class/actor/trait/import/sum/continuation
owners, runtime dynamic proxies, unmatched generic specialization, borrows,
unmodeled expressions, native allocation and suspension effects. Features
may be enabled only with real closure and lifetime proofs, not text scans.

Other compiler modes retain their previous behavior: the opt-in sealed profile
does not silently replace standard guest compilation, JIT modes, or existing
`--tree-shake`. The AOT script defaults to `off`; unknown profile strings
fail closed. The code-generation and no-GC IR auditors still run after
admission for AOT executable builds.

**Proof boundaries:** existing native allocator/GC IR audit is only an IR
screen, not process zero-GC or live actor cleanup proof. CI requires a
nonzero-step exact-head compiler invocation, linked executable digest and
runtime output. Hosted `steps=null` is **NOT_EXECUTED**.
