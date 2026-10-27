# Asynchronous I/O only: native LLVM transport contract

**Target architectural invariant — not an implemented capability.**
All source-visible core/standard-library I/O must be backed by asynchronous
operations. `_blocking` refers to a *logically waiting Oreslang continuation*,
not to a blocking host operation or an occupied scheduler carrier.

## Source and lowering semantics

| Source operation | Result | Lowering |
| --- | --- | --- |
| `fs.read_text(path)` | `Future<String>` | Admit async request, return future immediately |
| `await fs.read_text(path)` | `String` | Suspend caller and schedule continuation on completion |
| `suspends fnc f(): T` | `T` | May suspend for non-I/O language effects; does not create a blocking I/O API |
| `async fnc f(): T` | `Future<T>` | Returns immediately and may suspend within its task |

Guest I/O has one public spelling: the plain operation name. There is no
guest-visible `*_blocking`, `*_sync`, or parallel `*_async` I/O family.
Those spellings are rejected by the native compiler. A transport is classified
by typed effect/ABI metadata, not identifier suffixes, and imported aliases or
first-class call paths may not erase its async effect.

Every I/O operation must enforce permission/capability checks **before**
submission, bounded admission, ownership/borrow safety across suspension,
deadlines, cancellation, error typing, ordering, actor isolation and wakeup.
No guest callback may execute on an I/O completion thread. Parent/actor
control-plane work must not be starved by I/O traffic.

## Backend ABI target

1. Submit a typed I/O request with an owning task/actor, capability handle,
   data-buffer ownership and deadline; return an operation token.
2. Reactor/completion driver registers readiness/completion without synchronously
   waiting for results on a carrier. Use platform facilities appropriate to
   each operation: e.g. epoll/kqueue/IOCP for readiness/completion and io_uring
   for supported Linux file operations.
3. Completion atomically wins against timeout/cancellation/close exactly once;
   stale completions cannot access freed actor state or buffers.
4. Wake the *owning scheduler* by enqueueing a continuation; never run guest
   code inline on a kernel-completion or external-library thread.
5. The plain source operation exposes a `Future<T>`; callers use `await`
   when they need the result. No blocking or suffix-specific I/O lowering exists.
   It never lowers to `read`, `write`, `connect`, `recv`, `accept`,
   `poll`, `sleep`, `pthread_cond_wait`, `future.get` or equivalent
   blocking waits on an actor/ROOT carrier.

Kernel-level truly asynchronous *filesystem metadata, console, pipes and
regular files* are not equally available on every operating system.
A thread-pool workaround can preserve carrier-freedom but **does not** qualify
as an entirely nonblocking host-I/O implementation. Record that limitation
explicitly and reject the operation in a strict async-only target when its
backend does not provide a supported nonblocking mechanism. Never claim
`io_uring`, `AsynchronousFileChannel`, libdispatch, or offload workers
guarantee zero blocked OS threads on all platforms.

## Native completion terminal gate (partial implementation)

The standalone `runtime/AsyncIoCompletionGate.hpp` tracks a typed
`(actor_id, generation, operation_id)` identity and arbitrates
`pending -> success/failure/cancelled/timed_out/closed` via a single
atomic compare/exchange. Terminal transitions are single-winner and stale
actor-generation/op IDs are rejected. `tests/async_io_operation_test.cpp`
stresses 1,000 three-way completion/cancellation/timeout races.

This primitive does **not** retain the actor heap, own in-flight buffers,
install a reactor, schedule continuations or make callbacks safe after its
own allocation has been freed. Actual integration must pair it with a
separately verified lifetime gate and a priority-capable scheduler wakeup.
It is not evidence of native async I/O support.

## Current status and gates

Native LLVM on `main` has no imported standard-library I/O registry,
nonblocking transport implementation, async/await/suspends lowering, or task
continuation frames. The present compiler must reject all unsupported I/O
operations **without producing LLVM IR**. Existing `tests/fixtures/unsupported-native-io-{bare,blocking}.ores` and the
Rust async-only boundary matrix keep the current unsupported surface fail-closed.
The matrix also rejects legacy `*_async` spellings so a second API family
cannot appear accidentally.

Complete migration requires:
- Versioned shared typed I/O ABI/registry, with async-only backends and
  `suspends` effects, covering aliases/qualified method calls.
- Native reactor, pending operation ownership, buffer leases and actor-safe
  cancellation/close, plus genuine TCP/HTTP/filesystem/stdio backend work.
- Zero-carrier-blocking tests with one carrier and at least two actors; race
  tests for cancel-before-submit, timeout-vs-success, EOF, resource closure and
  port/FD reuse; quota and backpressure tests.
- Differential Java/LLVM fixtures proving the **same** results, errors,
  cancellation and suspension behavior for awaited plain I/O operations.
- Exact-head Clang/LLVM, CMake/CTest and Java/GraalVM test evidence, not
  documentation-only or tests from an earlier commit.

Java parity work now treats plain core/std I/O names as asynchronous by default.
That does not prove LLVM feature parity or that every host primitive is
kernel-nonblocking; each backend still needs exact-head behavioral evidence.


## Linux socket/readiness implementation slice (2026-10-12 draft)

The Linux-only \`NonblockingTcpLinux.hpp\` implements owned CLOEXEC/O_NONBLOCK
socket creation, a loopback listener, nonblocking accept4, MSG_DONTWAIT TCP
reads/writes, and epoll readiness with stable numeric tags. The standalone
\`native_linux_nonblocking_tcp_readiness\` CTest validates a real socket
round-trip, readiness, EAGAIN and owned FD flags. This is not source-level
\`std/net\`, compiler async lowering, a scheduler, or a production listener.
An epoll wait may park a *dedicated I/O reactor* thread, never an actor
or ROOT execution carrier. Completion callbacks may not touch actor memory.
Production adoption still requires bounded queues, operation-generation
validation, buffer lifetime retention, exactly-once completion, cancellation
and cross-backend differential tests.
