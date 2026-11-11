# AttoLang native HPC i64 tile overlap v1

**Status:** feature-branch compiler candidate, awaiting exact-head stepful source-local CI and independent review. This is **not** an HPC performance or full task-graph capability claim.

## Small implemented slice

- `tests/fixtures/hpc-reduction-v1/i64-fixed-tiles.atto` is an actual AttoLang guest defining four pure scalar tile sums and a fixed binary merge, expected native `main` exit 42. Canonical Git blob: `9deb9935b4f65b4d53de92f200291a28cdb02a79`, byte-identical to Java companion and platform conformance PR #704.
- `i64-task-tile.atto` creates the current compiler's one allowed `Task<int>` (capture-free pure integer producer), computes **one** immutable caller-local integer tile after submission, then awaits and merges the results. `SourceTaskRuntime.c` submits to its one-carrier native scheduler; the parent caller is a separate execution context. This enables a possible overlap interval, **not** evidence of physical simultaneous core execution.
- `Parser.cpp` permits only one preceding immutable scalar `int` local whose initializer is recursively capture-free and pure. A mutable `let` local, foreign buffer, guest capture, heap owner, second task, or arbitrary locals before await remain rejected.
- `tests/hpc_reduction_native_v1.rs` rejects dirty/uncommitted fixture bytes, compiles each positive guest through the actual LLVM + no-GC screen + Clang AOT driver at O0/O2, executes the generated native binary, requires exit 42, validates source and compiler hashes before/after, and rejects the mutable-before-await and second-task inputs with no executable publication.

## Run only from an exact Git checkout with actual compiler artifacts

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-no-gc-ir-audit-build --parallel 2
rustc --edition=2021 -D warnings tests/hpc_reduction_native_v1.rs -o /tmp/atto-hpc-v1-gate
EXPECTED_HEAD="$(git rev-parse HEAD)" /tmp/atto-hpc-v1-gate
```

Source-local GitHub Actions workflow `hpc-native-reduction-v1.yml` records exact head/tree, four compiled-guest executions and two negative compiler admissions when a real runner executes. `steps=null` or a zero-step failure is `NOT_EXECUTED`, never a semantic test result.

## Promotion blockers

1. Execute actual compiler/Clang/guest stages; preserve nonzero-step logs, SHA/tree/compiler hash and binaries. Both O0/O2 must match.
2. Check Java/Graal same-byte i64 source and separate f64 numerical cancellation checks on its own exact-head runner. Do **not** infer binary64 parity from integer execution.
3. Replace tiny constant-work fixtures with non-constant owned array tiles and real source loops, then implement multiple independent in-flight tasks and bounded cancellation/failure propagation.
4. Verify scheduler carrier independence, p50/p95/p99, CPU affinity/core IDs, workload scaling and matched serial baselines on physical CPUs. Inspect vector IR/disassembly before claiming SIMD, AVX/NEON or multi-core speedups.
5. Preserve existing actor single-owner memory semantics, generation fencing, critical sections and no raw cross-actor writes. Native task workers are not actors.

The semantic roadmap is tracked in platform-conformance issue/PR #215 and source LLVM issue #1138. GPU, NUMA, MPI, f64, SIMD, TaskScope and actor-parallel admission are **not** implemented by this PR.
