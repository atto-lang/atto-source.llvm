# AttoLang M1 opt-in LLVM compiler metadata — draft

Stacked on the separate M0 LLVM-native host-policy draft; companion to
conformance issue #323 / PR #326. The metadata flag **does not** create an MCP
server, guest callable reflection, or a native artifact containing descriptors.
It is a source-compilation metadata mode only.

`oreslang-llvmc --mcp-descriptors <source.atto|source.ores>` passes the input
through the normal checked source loader, then emits deterministic, sorted,
public-only scalar-function metadata. It is opt-in, 32 descriptors maximum,
versioned, and returns an error rather than leaking actor state, class layouts,
private functions, generic methods, source paths or unsupported constructs.
Numeric stable IDs are hex strings so JSON cannot lose 64-bit identity in a
JavaScript client. The M0 native resource table is separate and static.

Evidence levels remain distinct: host C++ unit tests can run without LLVM;
actual `.atto` and `.ores` compiler execution requires a built
`oreslang-llvmc`, then exact-head, stepful CI receipts. Embedding descriptors
in a native AOT guest artifact and binding them to generation / ABI / tenant
is a later increment; source-free tests are not such proof. Production MCP
transport and mutation remain forbidden.


## M1 native AOT descriptor retention (host ELF only)

The opt-in --mcp-embed-descriptors flag emits verified LLVM IR containing a bounded compiler-generated public-only descriptor in exported constant symbol __atto_mcp_descriptor_v1, section .atto.mcp.v1. The llvm.used retention list ensures the ELF linker preserves the section under --gc-sections. The artifact carries ABI atto.mcp.native.descriptor.v1 and a host_runtime_required generation marker; no runtime generation authority is inferred from compile-time data. No listener, reflection, JIT or guest invocation is introduced by inspection. The emitted metadata is capped at 8192 bytes plus NUL, rejects unsupported modes/targets, and cannot include private function names or source paths. The native test builds oreslang-llvmc, compiles and links .atto and .ores guest binaries, runs main (exit 42), extracts descriptor bytes using objcopy, checks deterministic schema and negative flag admission. Missing tools or zero-step CI are NOT_EXECUTED, not passes.

## Module identity, descriptor collisions and ELF read-only proof

The generated payload contains a deterministic 16-digit `module_id` derived from the checked namespace/module identity. It is deliberately non-authoritative: the live host must still resolve peer, tenant, boot and generation before serving any snapshot. The emitter rejects duplicate name/arity descriptors and duplicate opaque descriptor IDs rather than silently aliasing two ABI entries. The native artifact test now checks uniqueness, read-only allocated `PROGBITS` ELF section flags, exactly one descriptor section, and complete **absence** of descriptor data in an ordinary (non-opt-in) executable. The source-free linker canary has a negative mutation that makes the descriptor writable and must fail read-only admission. Compiler-backed `.atto`/`.ores` guest execution remains `NOT_EXECUTED` until a real source checkout, compiler build and native binary run produce exact-head receipts. The Python assertions explicitly refuse optimized `python -O` execution.

## Fail-closed compiler admission for metadata-only mode

The `--mcp-descriptors` metadata-only path now requires the fully verified LLVM IR lowering of the loaded native AST to succeed **before** it can write a descriptor manifest to stdout. The reserved LLVM native `abort` symbol is a regression: source parsing and metadata naming must not allow a manifest for a program that the backend rejects. The opt-in `--mcp-embed-descriptors` branch already runs full backend lowering before returning generated IR. The negative fixture requires both descriptor modes and default compilation to fail without producing output. This changes compiler admission, not guest/runtime execution evidence.
