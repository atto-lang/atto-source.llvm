# Compile-time performance baseline (LLVM backend)

Status: **measurement harness**, not incremental compilation or Java parity.
The current `oreslang-llvmc` parses one source file, checks its admitted
subset, emits verified LLVM IR and exits. It has no exported-interface
reader, incremental object cache, multi-module linker, generic
monomorphization, or debug/release compilation profiles.

## Baseline

After building the native compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R native_compile_benchmark_harness_selftest --output-on-failure

python3 scripts/benchmark_compile.py \
  --compiler build/oreslang-llvmc \
  --source tests/fixtures/forward-call-main.ores \
  --warmups 3 --samples 20 --json-out /tmp/oreslang-native-baseline.json
```

The script executes a **fresh child process per sample**, hashes IR without
retaining the entire output in memory, and fails on compiler failure, empty
IR, timeout, excessive output, changing source/compiler binaries, or
conflicting IR. It bounds repetitions, warmups, timeouts, and stdout/stderr
file size. On POSIX the child receives a kernel `RLIMIT_FSIZE` quota for each
output stream; on other operating systems the cap is checked after process
exit, so external tooling should additionally enforce disk quotas.
Each report includes the SHA-256 of both source and compiler binary, output
size, executable and arguments, wall-clock median and nearest-rank p95.
The hash checks detect lasting changes during a run but cannot prove that
untrusted build inputs were never transiently swapped. The warmups warm the OS and surrounding environment,
**not an in-process compiler cache**.

To compare cleanly, run the same source and compiler mode on the same
machine, pin CPU frequency where possible, record OS/hardware and compiler
commit separately, and use at least 20 samples. CI exercises correctness
of the harness only; it **does not gate on wall time**, which is noisy
across hosted runners. The isolation gate for the benchmark harness passed
6 unit tests in [GitHub Actions run 38519095796](https://github.com/canonical-cloud/canonical-mcp-server.rs/actions/runs/38519095796).
This is not evidence that the private LLVM compiler compiled guest source.

This script does not measure LLVM/Clang object-code optimization or final
binary linking. Measure those separately when a native object/link stage
exists. Java IncrementalCompiler warm-build numbers are also a different
metric and must not be equated with native fresh-process numbers.

## Follow-on architecture

1. Lock a cross-backend typed public-interface schema, versioned by
   compiler/target/feature and source identity, before attempting on-disk cache.
2. Support module import graphs and interface-only dependency checking;
   cyclic components require two-phase declaration collection, without
   executing module code during import.
3. Canonical type and generic specialization IDs, bounded solving, and
   unique overload selection; reject unsupported generics/trait/structural
   features until proved in both frontends.
4. Add a development codegen profile separate from release optimization;
   static semantic/ownership checks remain mandatory in all profiles.
5. Add deterministic per-module native object caching once binary/object
   format and cross-module ABI are stable; avoid caching raw IR using only
   a filename or unqualified type name.
6. Benchmark single-file parse, typed resolution, IR verification, native
   code generation, linking, warm single-body edits, ABI edits, and cache
   corruption/invalidation separately. Run differential Java/LLVM
   conformance fixtures before advertising parity.

The full language-feature design is tracked by the Java compiler's
`docs/COMPILE_TIME_ARCHITECTURE.md` on PR #924.
