# AttoLang freestanding negative helper-injection proof

This gate extends the compiler-backed `app.atto` Cortex-M pipeline and depends on
LLVM #1478 (reproducible compiler-owned `.atto` embeds). After compiling a
real `atto.core` guest to LLVM IR, the Rust gate mutates **that produced IR**
with an exported reachable function that calls an unrecognized generated
helper. It then:

1. confirms the IR declaration auditor detects the injected helper;
2. compiles the mutated IR for the same ARM target and checks `llvm-nm`
   exposes the unknown undefined symbol and the object ABI audit rejects it;
3. forces the injected probe live under `ld.lld --gc-sections`, requiring the
   **no-libc link** to fail with the injected helper name;
4. preserves the original successful QEMU boots and existing negative hosted
   C, allocator, std, ambient import and overflow tests.

CI checks compiler-owned generated `.atto` source exact bytes before the
build and requires both QEMU and mutation markers in its log, recording exact
PR SHA/tree and binary/fixture hashes in an uploaded receipt when steps run.
Artifact presence or green workflow metadata alone is not independently
qualified guest evidence: reviewers must validate run attempt, actual job
steps, source and negative results. A zero-step job is a runner admission
failure.

This does not prove arbitrary native code cannot call semihosting, the
complete Core APIs, or execution on physical Cortex-M silicon.
