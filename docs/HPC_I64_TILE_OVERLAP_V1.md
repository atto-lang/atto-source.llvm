# AttoLang native HPC i64 tile overlap v1

**Status:** feature-branch compiler candidate, awaiting exact-head stepful source-local CI and independent review. This is **not** an HPC performance or full task-graph capability claim.

## Small implemented slice

- `tests/fixtures/hpc-reduction-v1/i64-fixed-tiles.atto` is an actual AttoLang guest defining four pure scalar tile sums and a fixed binary merge, expected native `main` exit 42. Canonical Git blob: `9deb9935b4f65b4d53de92f200291a28cdb02a79`, byte-identical to Java companion and platform conformance PR #704.
- `i64-task-tile.atto` creates the current compiler's one allowed `Task<int>` (capture-free pure integer producer), computes **one** immutable caller-local integer tile after submission, then awaits and merges the results. `SourceTaskRuntime.c` submits to its one-carrier native scheduler; the parent caller is a separate execution context. This enables a possible overlap interval, **not** evidence of physical simultaneous core execution.
- `Parser.cpp` permits only one preceding immutable scalar `int` local whose initializer is recursively capture-free and pure. A mutable `let` local, foreign buffer, guest capture, heap owner, second task, or arbitrary locals before await remain rejected.
- `tests/hpc_reduction_native_v1.rs` rejects dirty/uncommitted fixture bytes, requires actual compiler-produced `__oreslang_guest_main` IR (and source Task ABI calls where applicable), compiles each positive guest through the real LLVM + no-GC screen + Clang AOT driver at O0/O2, executes the generated native binary, requires exit 42, and validates source and compiler hashes before/after. It also rejects *four* invalid inputs (mutable caller tile, two caller tiles, second Task, producer function call), checking both zero compiler-produced IR and no published executable.

## Run only from an exact Git checkout with actual compiler artifacts

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-no-gc-ir-audit-build --parallel 2
rustc --edition=2021 -D warnings tests/hpc_reduction_native_v1.rs -o /tmp/atto-hpc-v1-gate
EXPECTED_HEAD="$(git rev-parse HEAD)" /tmp/atto-hpc-v1-gate
```

Source-local GitHub Actions workflow `hpc-native-reduction-v1.yml` records exact head/tree, four compiled-guest executions and four negative compiler admissions when a real runner executes. `steps=null` or a zero-step failure is `NOT_EXECUTED`, never a semantic test result.

## Promotion blockers

1. Execute actual compiler/Clang/guest stages; preserve nonzero-step logs, SHA/tree/compiler hash and binaries. Both O0/O2 must match.
2. Check Java/Graal same-byte i64 source and separate f64 numerical cancellation checks on its own exact-head runner. Do **not** infer binary64 parity from integer execution.
3. Replace tiny constant-work fixtures with non-constant owned array tiles and real source loops, then implement multiple independent in-flight tasks and bounded cancellation/failure propagation.
4. Verify scheduler carrier independence, p50/p95/p99, CPU affinity/core IDs, workload scaling and matched serial baselines on physical CPUs. Inspect vector IR/disassembly before claiming SIMD, AVX/NEON or multi-core speedups.
5. Preserve existing actor single-owner memory semantics, generation fencing, critical sections and no raw cross-actor writes. Native task workers are not actors.

The semantic roadmap is tracked in platform-conformance issue/PR #215 and source LLVM issue #1138. GPU, NUMA, MPI, f64, SIMD, TaskScope and actor-parallel admission are **not** implemented by this PR.


## Full-width parameterized numerical differential gate (P0 follow-on)

The shared canonical source `tests/fixtures/hpc-reduction-v1/i64-parameterized-tiles.atto` has Git blob
`88795ff752006d020ea08c6585a8b3b03bf641c7` and is byte-identical in
`oreslang-source.java` PR #2900 and `oreslang-platform-conformance` PR #704.

`pub fnc tile_reduce8(int a, ..., int h): int` is ordinary AttoLang guest
code: two sequential four-element tiles, each `((a+b)+c)+d`,
then `tile0 + tile1`. A standalone Rust proof runner
`tests/hpc_i64_differential_v1.rs` compiles that source through the real
`oreslang-llvmc --target=host` frontend, verifies that LLVM IR defines the
actual external `i64 @tile_reduce8` and guest main, and runs the native no-GC
IR auditor. It renames only the generated operating-system `i32 @main`
wrapper to permit a restricted C observer to call the compiled guest
`tile_reduce8` ABI with **runtime-supplied** 64-bit integer parameters.
The observer parses and prints integer values; it does not implement a
reduction algorithm.

The Rust oracle performs checked i64 adds in the guest's precise grouping.
Its fixture corpus includes exact signed boundaries, cancellation, values
outside the 8-bit OS exit-status range, and 96 deterministic xorshift64 input
vectors seeded by `0x5EED_F00D_BADC_0FFE`. Total: **107 vectors at each
of LLVM/Clang O0 and O2, 214 required native guest executions**. A single
mismatch, IR-audit rejection, failed link or timeout fails the run. The gate
prints individual vector receipts and final source-tree/compiler/source/IR/
native-binary SHA-256 identities. The Java suite independently executes the
same 107 input values as Graal guest expressions in `AttoHpcTileGuestTest`.

The source-local CI workflow runs a Rust test-harness unit suite first, then
the pre-existing four O0/O2 fixed/one-task guest checks and four negative
admissions, then the parameterized 214-comparison differential gate.
Each requirement is independently checked; success of host-side Rust
unit tests or the C observer syntax screen must not be reported as guest
execution.

Manual source-local invocation (when rustc, Clang and the actual compiler
are available):

```sh
rustc --edition=2021 -D warnings --test tests/hpc_i64_differential_v1.rs \
  -o /tmp/atto-hpc-i64-diff-unit
/tmp/atto-hpc-i64-diff-unit
rustc --edition=2021 -D warnings tests/hpc_i64_differential_v1.rs \
  -o /tmp/atto-hpc-i64-diff
EXPECTED_HEAD="$(git rev-parse HEAD)" /tmp/atto-hpc-i64-diff
```

**Promotion status: NOT_EXECUTED until the compiler tests run on an allocated
runner with real nonzero steps.** At this stage there is no hardware speedup,
SIMD, multicore reduction scheduling, JVM/AOT equal-bit evidence, or f64 LLVM
source parity claim. A successful host C syntax check does not change this.
