# Native HTTP actor integration slice

This executable links LLVM-compiled `worker.ores` and its imported `http_status.ores`
helper with the native C actor
scheduler, actor-owned scalar heaps and a C++ HTTP/1.1 adapter. It requires no
Java, JVM, JIT, source parser or compiler at request time. Both scheduler workers
and the I/O reactor run inside **one OS process**. Actor messages never travel
over HTTP, pipes, sockets, subprocesses or shared-memory IPC.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 4
scripts/build-native-http.sh
build/native-http/native-http-server 3000 128 4
# another terminal
curl -i http://127.0.0.1:3000/health
python3 tests/native_http_actor_e2e.py --server build/native-http/native-http-server
python3 scripts/benchmark-native-http.py --server build/native-http/native-http-server \
  --output /tmp/native-http-benchmark --clients 100 --actors 128 --carriers 4 \
  --warmup 10 --seconds 180
```

Set `CLANG`/`CXX` to compilers compatible with the configured LLVM version if
needed. `SANITIZE=1 scripts/build-native-http.sh build/native-http-sanitized`
builds ASan/UBSan instrumentation. Only benchmark a Release artifact, with no
other builds/tests/load generators running.

## Ownership and scheduling

The listener is loopback-only, with at most 1,024 connections and one active
exchange per connection. The adapter parses bounded headers, freezes route
selection, publishes an exchange generation/actor identity, then sends a
runtime-private token through `NativeActorScheduler`. Rejected mailbox admission
rolls ownership back and retries later; it never invokes the handler directly.
The scheduled callback authenticates its current actor before claiming the token.
It enters that actor's own heap, initializes/executes the compiler-generated
handler, builds its response and publishes a completion into a bounded memory
ring. A release/acquire phase transition fences every request/response access.
Generation checks reject stale requests, completions and FD-slot reuse.

Only one callback executes per actor. Different actors run in parallel on the
carrier pool. Actor fields persist across requests. Both the reactor and actors
are native; guest handler code is LLVM-generated machine code. Every valid
application request, including 404/405, executes that handler. Invalid transport
framing is rejected before guest admission.

The reactor exclusively operates the socket. Actors receive exclusive request/
response **capability ownership**, not an unrestricted raw descriptor. After
response publication they cannot mutate that exchange. This is an in-process
ownership transfer, not `SCM_RIGHTS`, HTTP or cross-process IPC. A socket remains
registered with the reactor so that an actor never blocks on readiness or partial
writes. Kernel event notifications (`EVFILT_USER`/`eventfd`) only wake the driver;
no actor payload or descriptor is transported through them.

macOS uses kqueue and Linux uses epoll. Socket reads/writes/accepts are
nonblocking. Only the dedicated reactor may park in the kernel readiness wait;
idle scheduler workers may park awaiting mailbox work. There is no filesystem
I/O or stdout/stderr export during request handling. Startup and final benchmark
report output are host lifecycle operations. Unsupported filesystem APIs are
not exposed by this slice; there is no blocking-worker fallback.

## Deliberate limits

**This is not full Spin compilation or GraalVM feature/performance parity.**
The native guest actor increments a persistent request count and passes the
adapter-selected status through a statically imported public function and a
private scalar class method before returning it. Parsing, fixed routing and response formatting
are trusted C++ work. Only bodyless HTTP/1.1 requests are admitted; `/health`
returns `ok\n`; other GET paths produce 404 and other methods produce 405.
Request bodies, TLS, HTTP/2, general routing, async guest handlers, cancellation,
imports, exceptions and the Oreslang OTel module are not implemented here.
It does not execute the JVM benchmark's guest suspend/cooperate checkpoints.

Each actor has a separately allocated, domain/epoch-checked native scalar heap.
All actors share one process/address space. This is runtime ownership isolation,
not hardware memory protection or a sandbox against malicious C/FFI. Shared vs
private actor source semantics still require compiler integration; relabeling
the same host harness would not establish that comparison. No CPU affinity
claim is made for this adapter.

The benchmark reuses 100 connections and 128 actors for 180 measured seconds,
after 10 warmup seconds. It reports HTTP latency separately from the complete
client cycle (including client scheduling), exact failures and completion counts.
Server phase means include warmup and are labeled accordingly. Server queue time
means parsed request to admitted callback start; it excludes incomplete-header
network wait. Native telemetry records aggregates in memory and exports after
drain; it is not yet oreslang-otel or OTLP. Read this workload difference before
comparing numbers with the full JVM Spin application.
