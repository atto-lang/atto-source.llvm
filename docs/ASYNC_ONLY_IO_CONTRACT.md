# Asynchronous I/O only: native LLVM transport contract

**Target architectural invariant — not an implemented capability.**
All source-visible core/standard-library I/O must be backed by asynchronous
operations. `_blocking` refers to a *logically waiting Oreslang continuation*,
not to a blocking host operation or an occupied scheduler carrier.

## Source and lowering semantics

| Source operation | Result | Lowering |
| --- | --- | --- |
| `fs.read_text_async(path)` | `Future<String>` | Admit async request, return future immediately |
| `await fs.read_text_async(path)` | `String` | Suspend caller and schedule continuation on completion |
| `fs.read_text_blocking(path)` | `String` | Admit the **same async request**, implicitly suspend caller |
| `suspends fnc f(): T` | `T` | May suspend; effect propagates through typed calls |
| `async fnc f(): T` | `Future<T>` | Returns immediately and may suspend within its task |

A transport is classified by typed effect/ABI metadata, not arbitrary
identifier suffixes. User-defined pure functions may happen to end in
`_blocking` and must not be mistaken for native I/O. No `async suspends` pair.
A native I/O `_blocking` operation *always* carries a suspension effect,
including method and imported-alias calls. It cannot be smuggled through an
untyped callback, first-class function pointer or FFI declaration. The checker
must either preserve its effect or reject the call.

Both API styles must share permission/capability enforcement **before**
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
5. `_blocking` lowers to that submission plus implicit continuation yield.
   `_async` lowers to the same submission and exposes a `Future<T>`.
   Neither lowers to `read`, `write`, `connect`, `recv`, `accept`,
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

## Current status and gates

Native LLVM on `main` has no imported standard-library I/O registry,
nonblocking transport implementation, async/await/suspends lowering, or task
continuation frames. The present compiler must reject all unsupported I/O
operations **without producing LLVM IR**. Existing
`tests/fixtures/unsupported-native-io-{bare,blocking}.ores` already guard
two cases; the async-only boundary matrix widens the future regression gate.

Complete migration requires:
- Versioned shared typed I/O ABI/registry, with async-only backends and
  `suspends` effects, covering aliases/qualified method calls.
- Native reactor, pending operation ownership, buffer leases and actor-safe
  cancellation/close, plus genuine TCP/HTTP/filesystem/stdio backend work.
- Zero-carrier-blocking tests with one carrier and at least two actors; race
  tests for cancel-before-submit, timeout-vs-success, EOF, resource closure and
  port/FD reuse; quota and backpressure tests.
- Differential Java/LLVM fixtures proving the **same** results, errors and
  suspension behavior for `await *_async` and `*_blocking`.
- Exact-head Clang/LLVM, CMake/CTest and Java/GraalVM test evidence, not
  documentation-only or tests from an earlier commit.

Related Java work: [suffix/continuation PR #522](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/522)
and [HTTP sendAsync migration PR #584](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/584).
Neither proves LLVM feature parity or that all Java host I/O is nonblocking.
