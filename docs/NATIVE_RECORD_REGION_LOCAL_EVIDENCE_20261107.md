# Native record-region exact-blob local evidence — 2026-11-07

**Evidence class: HOST_EXECUTED_LOCAL_EXACT_BLOBS**. **Not hosted CI. Not
LLVM compiler-backed AttoLang guest execution. Not five-backend parity.**

## Scope and immutable identities

Verified the Git blob SHA-1 computed with `git hash-object` for three files
independently fetched from draft [LLVM #1520](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1520):

| Path | Verified Git blob |
| --- | --- |
| `src/main/cpp/oreslang/runtime/NativeRecordRegion.c` | `59604bf7035369217c04c1554ac37f27babda761` |
| `src/main/cpp/oreslang/runtime/NativeRecordRegion.h` | `75e4a679663371768cbae8abece9d3fb338cc8e6` |
| `tests/native_record_region_test.c` | `75ee5fc43d414987dcff0fdba00f9fbb5ce51004` |
| `scripts/test-native-record-region-mutations.sh` | `e9bce376ef994a61b47f4422ad6e803481e7d255` |

This proves the tested **individual source/test blob bytes** match the PR,
not that an entire GitHub checkout was cloned or a GitHub Actions runner
executed. Local environment: Debian Linux, `cc (Debian 14.2.0-19) 14.2.0`,
`javac 21.0.12.1` (Java not used for the native C test).

## Executed native tests

From the exact C source/test blobs, with `-std=c11 -Wall -Wextra -Werror`,
`-pthread -DORESLANG_RECORD_TEST_ALLOC_FAILURE=1` and the correct include root:

- Native test binary: **PASS**, output:
  `PASS record generation, writable masks, stable mutation, schema, quota, thread confinement and failure cleanup`.
- Recompile with `-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g`,
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1`: **PASS**, same output.
- Deliberately remove the writable-field permission mask check from a copy:
  native test **rejected mutation**, exit **134** (failed `ores_record_set`
  rejection assertion).
- Deliberately remove the invocation generation comparison from a copy:
  native test **rejected mutation**, exit **134** (failed cross-thread
  `ores_record_set` rejection assertion).

The script `scripts/test-native-record-region-mutations.sh` reproduces
baseline + both negative mutations on a normal shell/C toolchain using
`bash scripts/test-native-record-region-mutations.sh`. It requires an
assertion failure to count a killed mutant; an unrelated crash or build error
does not count as negative evidence. ASan/UBSan is a separate explicitly
executed local command, not implicitly part of the script.

## Outstanding release gates

1. Origin-org source-free Actions runner admission is still broken; latest
   unrelated source-free retry ended with `steps=[]` and 404 `BlobNotFound`
   logs, tracked on Java issue #963.
2. Execute this exact PR head/tree in a real GitHub Actions runner with
   nonzero steps and a recorded run attempt, job ID, compiler, and logs.
3. Re-run native CTest and sanitizers in the repository's exact full tree
   (this local proof covers the individual blobs, not all CMake integration).
4. Add an independent LLVM compiler-backed `.atto` guest fixture proving
   mutable scalar records after array/class collection growth, with
   generation and out-of-owner rejection; this PR currently only adds
   native ABI groundwork.
5. Independent valid reviewer approval before draft-to-ready and merge.

**Security:** no cross-org private-source mirror was performed. Do not infer
authorization to move compiler source from this local evidence.

## Additional local CTest registration check

An opt-in native-host CTest `native_record_region_negative_mutations` is
registered alongside the existing `native_record_region_generation_quota_cleanup`
when building on UNIX with GCC/Clang and `bash` available. The mutation
script is executable in the PR's Git tree.

The CMake fragment was reproduced in a **minimal local CMake project**, using
the verified exact source/test/script blobs (rather than a checkout of the
complete LLVM repository), then built and executed:

- `cmake -S <minimal-project> -B <build> -DBUILD_TESTING=ON`: **PASS**
- `cmake --build <build>`: **PASS**
- `ctest --test-dir <build> --output-on-failure -V`: **2/2 PASS**
  - `native_record_region_generation_quota_cleanup`: PASS
  - `native_record_region_negative_mutations`: PASS
- Negative mutations triggered native assertions and exit 134 as expected,
  which the test shell treats as deliberately killed mutants rather than
  successful mutated behavior.

**Important:** this demonstrates the isolated CMake registration syntax and
replayability. It does not establish that the *full* repository's 313k-byte
`CMakeLists.txt`, complete LLVM compiler build, or a hosted GitHub Actions
job configured/executed successfully.
