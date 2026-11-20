# Full-AOT target for GraalVM and LLVM

Full AOT means compiling the application and its reachable dependencies into
native code before launch, with no source evaluator or runtime JIT required to
serve requests. Runtime allocation, garbage collection, dynamic dispatch,
mailboxes and async I/O remain valid; they are runtime services, not JIT work.

The current GraalVM `native-aot` profile compiles the Java interpreter into a
native image and disables guest JIT. Its `--build-info` identifies guest execution
as `interpreter-no-jit`. Packaging Ores sources inside that binary does not make
them native guest code. The current LLVM compiler emits native guest code for
its admitted subset, but cannot yet compile the Spin framework. Neither is a
full-AOT Spin deployment today.

## Compile-time contract

An eventual explicit full-AOT build mode must reject unsupported reachable
constructs at build time, freeze the module/route graph and backend capability
manifest, and contain no fallback interpreter. Hybrid remains a separate build
choice. Launch flags must not silently change an AOT-only artifact into a JIT
artifact. These are requirements, not implemented CLI flags.

## Work sequence

1. Establish executable output and run it without a compiler or Java on PATH.
   `scripts/compile-aot.sh` implements this for the admitted LLVM host subset.
2. Define a shared typed, resolved application representation and conformance
   corpus. Resolve imports, methods and suspension effects before request handling.
3. GraalVM: lower that representation to guest-specific Java/Truffle execution
   code that Native Image can compile. Building only the AST evaluator is insufficient.
4. LLVM: extend guest lowering for imports, objects, ownership, errors and the
   standard-library operations actually reached by Spin. Host-only actor tests
   are not evidence of guest actor lowering.
5. Lower async methods to explicit continuation state machines on both backends;
   preserve one executing callback per actor, cancellation, completion and mailbox
   capacity semantics. No OS-thread blocking at each await.
6. Implement/link native actor, memory/GC, HTTP and I/O services with the same
   ownership-transfer guarantees. Compile shared and isolated handlers separately
   from the supervisor; do not parallelize a single actor's mutable state.
7. Gate full-AOT Spin artifacts on routing semantics, actor exclusivity and transfer
   tests, telemetry, and 180-second 100/1,000/10,000-client runs. Record latency,
   throughput, failures, queueing and memory alongside the exact artifact hash.
8. Prove zero runtime compilation and zero source parsing/evaluation while serving;
   compare cold and warm runs without attributing unrelated transport changes to AOT.

The serial supervisor bottleneck also needs direct attention: eliminating
repeated suspension analysis and structural AST hashing helps today's runtime,
but a compile-time artifact should remove those analyses from serving entirely.
Actor-per-request still traverses a serial supervisor when routing is performed
there; changing actor lifetime alone does not remove that serial work.
