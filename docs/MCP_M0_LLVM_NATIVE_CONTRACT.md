# AttoLang MCP M0 LLVM-native host policy — draft parity slice

Companion to `ores-truffle-oreslang/oreslang-platform-conformance` issue #323
and draft PR #326. This is a **native C++17 host policy/reference admission**
implementation in the LLVM backend repo, not executable AttoLang guest code.
There is **no MCP socket/listener, OS peer-credential adapter, wire JSON parser,
server authentication, guest reflection, runtime control API or upgrade method**.

The M0 descriptor table is `constexpr` and finite: five redacted, read-only
resource **names**, an explicit scope per resource, and stable versioned FNV-1a
IDs. No private state, raw pointer, credential or source map is retained.
This **is not yet the compiler-generated per-program M1 descriptor registry**.
A future M1 integration must generate and tree-shake descriptors from validated
AttoLang sources, bind them to the artifact ABI and generation, and prove they
survive native AOT linking without runtime reflection/JIT or interpreter.

`Ledger::begin` requires an already-authenticated host peer, host-owned
credential and boot/generation identities. It guards a bounded array of
per-principal attempts, nonces and request tickets behind a native mutex; no
suspension or guest callback is permitted inside it. It enforces quotas
(8/second, 60/minute, 2 in flight, 64 principals, 128 retained nonces),
nonce replay, typed request IDs (string vs integer), explicit resource/scope,
tenant and runtime matching, cancellation-safe one-shot ticket closure,
monotonic deadline and credential-expiry fences. Authenticated denials consume
rate capacity. Unknown methods, arbitrary evaluation and upgrades fail closed.

Audit callbacks are trusted-host inputs and must be synchronous, nonblocking
and durable-before-acknowledgement. No audit sink/durability is implemented in
this package: tests simulate a successful callback and failure injection.
The wire length/depth values are passed by a **trusted upstream parser**;
real duplicate JSON-key, Unicode, framing, timeout, disconnect and invalid
UTF-8 checks must still be wired in before any production listener is enabled.

## Evidence tiers

- Source-only JSON manifest test: contract parity of budget/constants/resources.
- Native host C++17: 310 independent checks, GCC and Clang optimized builds.
- Native sanitizer: GCC and Clang with ASan+UBSan are separate local evidence.
- Compiler-backed AttoLang guest fixture: **NOT_EXECUTED / NOT_IMPLEMENTED**.
- Native AOT embedding of compiler-generated descriptors: **NOT_EXECUTED**.
- Actual MCP client/transport, OS authentication and audit: **NOT_IMPLEMENTED**.
- Cross-backend parity: **NOT_ESTABLISHED**; this PR provides an LLVM-native
  policy baseline against the JavaScript source-free conformance reference.

GitHub checks are only admitted if a runner executes nonzero steps, and reports
must pin exact commit + root tree. Do not merge merely because source-free or
host-only tests pass. Keep independent review and private-source export gates.
