# AttoLang → LLVM native AOT bootstrap

This backend is an **actual compiler**, not a source-only placeholder. It uses an exact-SHA-vendored copy of the Rust-hosted Atto lexer, parser and typed checker, and adds a separate Rust LLVM IR emitter in `src/main.rs`.

## Build and run

On Linux x86-64 with Rust 1.85.1 and LLVM Clang 18:

```sh
cargo test --offline
cargo build --offline --release --bin atto-llvm
target/release/atto-llvm tests/fixtures/167-quic-varint-rfc9000.atto -o /tmp/atto-guest
/tmp/atto-guest
echo $? # 42 when the Atto guest's 308 checks all pass
```

LLVM IR alone can be emitted with `--emit-llvm`. The compiler also invokes Clang directly for an ELF executable. It does **not** emit Wasm, spawn a Wasm interpreter, execute a JIT or run user Atto source through a host-language interpreter. The native binary uses stack locals and checked signed i64 arithmetic; the admitted scalar execution profile has **no guest heap or garbage collector**.

The actual supported guest subset is: top-level functions with `int`, `bool`, and `void`; top-level calls; immutable/mutable locals; integer literals and checked neg/add/sub/mul; integer/bool comparisons and eager boolean operators; direct conditional expressions and `if` branches, plus `return`. Unsupported strings, floats, classes, actors, asynchronous I/O, heap allocations, exception handling, network operations and generators fail compilation with `E_UNSUPPORTED_NATIVE_*`. **This does not establish a zero-GC guarantee for any future actor runtime or complete language implementation.**

## Pinned frontend provenance

`frontend/` contains a 16-blob exact Git-object copy of the frontend rooted at `atto-lang/atto-source.rs@860137bcdc50684ab6cb6aa8f89932506b3beec4` (Cargo files and all required `src` modules). The original type-checker Git blob is `18c9b7e3e9696bfdd56fa5130f52a9a71a796c43` and parser is `6231be661be9b7bb40bd9ed69d4cd60ccc5b9609`. This is shared frontend implementation, not an independent second language parser. Runtime/code generation are separate.

## Verification

`bash ci/test_native_aot.sh` builds and runs the same 1,255 native `.atto` QUIC/HTTP3 pure-function checks that passed via Rust-to-Wasm in the platform conformance suite:

- 308 RFC 9000 QUIC varint checks
- 435 RFC 9114 frame/SETTINGS/bounds checks
- 256 request message frame-state checks
- 256 push message frame-state checks

It verifies identical LLVM IR emissions, ELF64 native outputs, return code 42, invalid source rejection, checked arithmetic traps, and ASan/UBSan smoke coverage. All fixtures are SHA-identical to the conformance source. The private alternate runner `athlet-o/athleto-e2e` can build an exact Git tree mirror using Git blob/tree APIs and its own GitHub Actions minutes—**no cross-organization token or secret**.

## Outstanding gates

No actual HTTP/3 client/server, QPACK parser, TLS1.3/QUIC transport provider, stream I/O, actor mailbox or per-actor arena/stack runtime has been implemented here. Full-AOT **scalar** proof does not imply full-actor AOT or five-backend semantic parity. Before claiming production networking, implement the ABI, actor-owned handles and buffers, TLS certificate validation, ALPN `h3`, QPACK, streaming, cancellation, fail-closed unsupported features, and independent network interoperability tests.

**Evidence hygiene:** guest compiler success and source blob parity alone are not HTTP/3 wire functionality. This remains a draft implementation until the missing runtime and interoperability gates pass.
