# Spin native LLVM parity delivery gates

Target: one OS process, native guest machine code and C/C++ runtime services;
100 persistent clients, at least 10K successful responses/sec, and mean pending
queue below 3 ms for 180 measured seconds. No JVM/JIT/interpreter or actor IPC.
HTTP clients still use TCP to reach the server; internal actor invocation uses
in-memory mailboxes. `HttpClient.send`/`sendAsync` are outbound HTTP client APIs,
not actor handle transfer. Separate actor heaps can occupy the same address space.

| Step | Deliverable | Current evidence |
|---|---|---|
| 1 | Fetch and reconcile current native main | Based on `d9c2f1c` (includes the scheduler/actor-graph bridge) |
| 2 | Build a standalone compiled guest actor | `examples/native-http/worker.ores` emits init/on_message machine code |
| 3 | Connect nonblocking HTTP to existing native actor mailboxes | macOS kqueue / Linux epoll adapter; one process |
| 4 | Fence exclusive request/response ownership | generation + actor identity; mailbox rejection rollback; completion release/acquire |
| 5 | Verify reuse, malformed framing and deterministic retirement | real TCP tests in Release and ASan/UBSan, separately allocated actor heaps |
| 6 | Establish 180-second native transport baseline | reproducible benchmark with artifact/source hashes; results published separately |
| 7 | Resolve the actual Spin import/type graph ahead of time | **Partial**: relative modules, typed strings/arrays, owned nested aggregates, and scalar-payload Option/Result compile; real RequestPath/PathCapture fixtures run. Class-payload sums, borrowed subobject results, and the complete route graph remain |
| 8 | Lower typed HTTP exchange/response capabilities into guest ABI | **Remaining**: today request parsing/response formatting is native adapter work |
| 9 | Lower Future/await/cooperate/cancellation continuations | **Remaining**: native scalar handler does not exercise JVM suspend checkpoints |
| 10 | Integrate shared/private source actor placement and capability policy | **Remaining**: separate native heaps are not hardware address-space isolation |
| 11 | Lower routing, HTTP and OTel stdlib modules | **Remaining**: host aggregate timing is not oreslang-otel |
| 12 | Run identical Java/native fixtures and sustained performance matrix | **Partial**: module/method/bool programs execute identically in Java/native; HTTP guest await/cooperate work still differs |

The initial adapter is an executable integration milestone, not a replacement
Spin implementation or proof that all Spin source now compiles. Unsupported
source remains rejected without fallback interpretation. See
[build and boundaries](../examples/native-http/README.md).
See [native sum storage and remaining lifetime work](NATIVE_SCALAR_SUMS.md).

Only the dedicated I/O driver may block in a readiness wait. No actor carrier
waits on socket readiness, performs synchronous filesystem I/O, or writes logs
to stdout during requests. Unsupported strict-NIO operations must fail explicitly;
worker-pool offloading does not qualify as native nonblocking filesystem support.
Idle scheduler condition waits and external teardown joins are lifecycle/scheduling
operations, not synchronous guest I/O.
