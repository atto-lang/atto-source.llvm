# Oreslang LLVM backend

Native source-language compiler and runtime companion to
[oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).

**Status: experimental first compiler slice, NOT source-language parity.**
Tracks [port/HPC roadmap issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).
Native/HPC rationale originates in
[Java WIP PR #470](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/470).

## Current functionality

The `oreslang-llvmc` binary parses exactly one zero-argument
`fnc`/`routine` returning a positive signed-64-bit integer literal
(optionally within one `define module ... as ... end`), generates LLVM IR,
validates it using `llvm::verifyModule` and prints it.

Unsupported expressions, classes, actors, imports, async/await,
recursive/owned objects, networking and native scheduling **fail closed**.
This is a first testable parser-to-IR path, not a runnable language implementation.

## Build and test

Requires LLVM development libraries 14+, CMake 3.20+, and C++17.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/oreslang-llvmc tests/fixtures/constant-main.ores
```

No user-facing flags are currently parsed; the sole positional argument
is an Oreslang source file. Runtime execution and object emission are future work.

## Source mapping

- `src/main/cpp/oreslang/frontend` — tiny lexical/AST front end; compare Java `parser` + `ast`.
- `src/main/cpp/oreslang/types` — reserved for Java `types`/borrow parity.
- `src/main/cpp/oreslang/backend` — LLVM IRBuilder+verifier in lieu of Truffle `nodes`.
- `src/main/cpp/oreslang/runtime` — reserved for scheduler/actors/futures, stable frames and arenas.
- `tests/fixtures` — initially source and IR smoke tests; eventually shared Java parity cases.

See [source parity contract](docs/SOURCE_PARITY.md) and
[Java managed cyclic graph PR #474](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/474)
for the intentionally separate JVM/native ownership paths.

## Safety/status boundaries

Native pinned async frames, per-group GC/arena rotation, worker-core pinning,
NIC bypass, zero-GC HOT sections, native cyclic arena ownership and other
HPC/HFT targets are **roadmap items only**. Nothing in this repository
currently provides those features.
