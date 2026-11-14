# atto-source.llvm

Native LLVM backend for AttoLang. The current draft implements a **real compiled scalar subset**, using the shared checked frontend and direct LLVM IR generation. It does not execute guest code inside a Rust interpreter or WebAssembly engine.

## Quick start

Requires Rust 1.85.1 and LLVM/Clang (verified on Clang 18.1.3, Linux x86-64).

```sh
cargo build --offline --release --bin atto-llvm
target/release/atto-llvm tests/fixtures/167-quic-varint-rfc9000.atto -o /tmp/native-atto
/tmp/native-atto
echo $? # 42 means its 308 Atto checks passed
```

Use `--emit-llvm` after `-o output.ll` to inspect the generated LLVM IR. See [Native AOT implementation and limitations](docs/native-aot.md) for admitted types, explicit unsupported diagnostics, toolchain provenance and security scope.

## Verified native execution

[Exact-tree private alternate-runner AOT and sanitizer run](https://github.com/athlet-o/athleto-e2e/actions/runs/43649930185) passed 1,255 pure native `.atto` QUIC/HTTP/3 codec checks, four compiler unit tests, checked signed add/sub/mul/neg overflow traps, actual direct `atto-llvm` → ELF output, and ASan/UBSan smoke tests. The source snapshot's exact Git tree is `d02b401e5c7a3a524c4a70db8db8520c6ae408ee`, with a pinned 16-blob frontend copied from `atto-source.rs@860137bcdc50684ab6cb6aa8f89932506b3beec4`.

Conformance receipt: [`contracts/native-llvm-aot-v1.json`](contracts/native-llvm-aot-v1.json). Tests are real *native machine-code* execution on Linux x86-64. The admitted scalar profile has no guest heap or guest GC.

## Not yet implemented

Actor-owned heaps and stacks, supervisor lifecycle, a full-AOT actor runtime, real QUIC/TLS/UDP, QPACK, HTTP/3 client/server loopback, Windows/macOS/embedded profiles, and the full language lowering are not implemented. Browser/Node WASM, JVM/GraalVM and .NET are independently tested only when their own backends execute; identical `.atto` source files do not prove runtime parity.

The proposed 11-operation actor-safe transport boundary is in `contracts/http3-host-abi-v1.json`. This is an ABI specification **not** a network engine. Keep the PR draft until actual actor and network interoperability evidence exists.
