# LLVM native platform-core-v1 exact guest gate

The `contracts/platform-core-v1/` candidate corpus is Git-blob-identical
to the Java and Wasm copies. Rust has a candidate import in
[rs #70](https://github.com/ores-truffle-oreslang/oreslang-source.rs/pull/70).
This gate consumes the **unchanged** checked-in `.ores` bytes and must not
use a host-side arithmetic oracle as a substitute for compiled guest code.

`tests/platform_core_v1_original_native.cmake` has two modes:

- `validate`: checks exact corpus Git blobs and manifest structure **only**.
  It does **not** compile or run a guest.
- `native`: requires a specific source commit, full checked-in corpus,
  a binary-SHA256-pinned actual LLVM compiler and Linux Clang/objcopy.
  It compiles each original positive source to LLVM IR, checks exported
  guest and host wrapper symbols, renames only the compiled **OS wrapper**
  symbol (never the source expression), links the fixed native observer
  `tests/atto_i64_original_guest_probe.c` and executes on both O0 and O2.
  The oracle receives expected i64 via a command-line argument and reports
  42 on equality, 43 on mismatch. All seven positive outputs must agree.
  Unresolved symbols, invalid implicit conversion, removed caret, and
  missing return must fail compilation; checked overflow must compile and
  trigger x86-64 SIGILL rather than wrap.

Example on an **authorized source-local Linux x86-64 runner** after the
compiler has been compiled from the exact pinned source checkout:

```sh
cmake \
 -DMODE=native \
 -DSOURCE_DIR="$PWD" \
 -DEXPECTED_SOURCE_SHA="$(git rev-parse HEAD)" \
 -DCOMPILER=/absolute/path/to/oreslang-llvmc \
 -DCOMPILER_SHA256=REVIEWED_LOWERCASE_SHA256 \
 -DCLANG=/absolute/path/to/clang \
 -DOBJCOPY=/absolute/path/to/objcopy \
 -DSHIM="$PWD/tests/atto_i64_original_guest_probe.c" \
 -DWORKDIR=/absolute/path/to/isolated-test-workdir \
 -P tests/platform_core_v1_original_native.cmake
```

This script does not itself build or certify the compiler. The CI job must
independently attest actual compiler provenance, source head/tree, `ctest`
run attempt, nonzero executed steps, toolchain digests, immutable fixture
bytes and exit/diagnostic evidence. A native SIGILL is a provisional checked
arithmetic trap, not a typed Oreslang Error/supervisor CONTROL notice.

A successful native scalar test is **not** evidence of Java/LLVM global
parity, Rust compilation, Wasm portability, actor/task/ownership semantics,
hardware ABI, FPGA synthesis or physical ASIC signoff.

No cross-org private source transfer is authorized under conformance
incidents #22/#77. Source-org runner admission and protected `main`
remain hard prerequisites before marking this draft ready or merging.
