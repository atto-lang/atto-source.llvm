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

## Bounded native actor runtime (implemented and tested)

The LLVM backend now compiles a limited legacy `define actor` / `spawn` / `ActorMail<int|bool>` / `send` / receiver-method profile to real native ELF machine code, including per-actor state:

```sh
target/release/atto-llvm tests/actors/pass/180-two-types-state.atto -o /tmp/atto-native-actors
/tmp/atto-native-actors  # exit 0; per-actor FIFO output: 1,3 and 102,105
bash ci/test_native_actors.sh
```

[Native actor/arena/mailbox private-runner PASS](https://github.com/athlet-o/athleto-e2e/actions/runs/43657189906): 4 true native `.atto` actor fixtures × 20 runs (80 executable runs), 3 fail-closed compiler fixtures, 5 × 1,600 concurrent C ABI messages (8,000 accepted) through eight actors, and ASan/UBSan; all **1,255 previous scalar protocol tests** also remained green. The exact compiled source tree was `f40cb5044af7ffb55cbd05dd7024cc7963d6042d`.

The bounded runtime uses a 64 KiB zero-initialized actor-owned arena, a dedicated 256 KiB pthread stack, a 128-message bounded FIFO mailbox, 32-bit generation-checked actor handles, and explicit deterministic draining and teardown. It has no guest heap collector. This is one OS thread per actor, **not** the final many-actor cooperative/work-stealing scheduler.

Full semantics, limitations, executable examples, and the exact Git receipt: [Native actors](docs/native-actors.md) and [`contracts/native-llvm-actor-aot-v1.json`](contracts/native-llvm-actor-aot-v1.json).

## Not yet implemented

The **new** `use namespace` / `declare actor` / `mtd` native lowering, required sealed-state `handle_state(critical-read/critical-write)` enforcement, borrow checker across suspension, supervisor panic CONTROL, typed `send` Result, per-actor cancellation/close and child spawning, work-stealing scheduler, full language AOT, QUIC/TLS/UDP, QPACK, HTTP/3 client/server loopback, and Windows/macOS/embedded profiles remain unimplemented. Browser/Node standalone WASM, JVM/GraalVM and .NET must each execute their own backend tests; byte-identical source does not establish parity.

The proposed 11-operation actor-safe transport boundary in `contracts/http3-host-abi-v1.json` is a **specification**, not a working QUIC engine. Keep this PR in draft until the missing actor semantics and networking integration are verified.
