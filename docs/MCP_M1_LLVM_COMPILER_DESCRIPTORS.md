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
