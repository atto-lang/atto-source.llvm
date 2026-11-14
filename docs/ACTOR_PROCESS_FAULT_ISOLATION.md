# Native Atto process fault isolation — initial host substrate

## Implemented scope

`native/` contains a C11/POSIX **host demonstration** of a crash containment
boundary, independent from the legacy Oreslang LLVM runtime and independent of
Atto guest-code generation. Each `atto_actor_run_isolated()` invocation creates
an external worker through `posix_spawn()`; the supervisor does not call a guest
function after `fork()` on a multithreaded runtime. A crash is reported as a
structured notice identifying actor, supervisor, and incarnation. A trusted
caller may translate the notice into its own priority supervisor CONTROL
message, but **that mailbox integration is not implemented here**.

The typed results distinguish clean exit, nonzero exit, signal termination,
timeout, failed spawn, and failed wait. A SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, or SIGKILL in one worker does
not terminate its supervisor process. A concurrent healthy worker completes
while a second worker crashes. The worker gets only PATH/LC_ALL in its explicit
environment. The parent waits on that one child PID, not `waitpid(-1)`.

## Explicit non-guarantees

This is **not** a security sandbox or a complete actor implementation. It does
not integrate Atto source actors, mailboxes, ownership/borrow checking,
restarts, exception hooks, actor heaps, arena quiescence, or AOT lowering. A
worker can still inherit open file descriptors and other OS privileges, or
signal same-UID processes; a production untrusted-worker policy requires
close-on-exec/default-close, sandbox confinement, an authenticated IPC channel,
per-worker CPU/memory quotas (cgroups/job objects), and supervisor-controlled
resource retirement. It cannot recover from SIGKILL of the supervisor process,
whole-machine OOM, or compromised trusted host code. There is no recovery by
continuing execution in a SIGSEGV signal handler.

The current implementation is intended for POSIX/Linux/macOS hosts; embedded,
Windows NativeAOT, GraalVM, Wasm, and other runtime backends require their own
independent platform-specific implementation.

## Compile and verify

```sh
cmake -S native -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The test spawns an external executable and checks isolated SIGSEGV, SIGBUS,
SIGILL, SIGFPE, SIGKILL, SIGABRT,
nonzero and clean exits, a hung worker killed at a bounded deadline, inherited
environment sanitization, and a healthy concurrent sibling. Expected child
signals are intentional fault injection; do not interpret an individual child
crash as a failed parent-host test.

## Integration requirements

- Add an authenticated supervisor CONTROL lane to report the notice once.
- Support concurrent receiver quiescence and actor-owned arena retirement
  *after* verified last-use; **never** free actor-owned memory merely because
  `waitpid` reports a worker has died if other actors can hold loans.
- Move from a trusted worker executable to a sandboxed worker with strict
  inherited-FD policy, memory/cpu budgets, mailbox IPC, and control-plane
  admission. Guard fork+exec/posix_spawn semantics on each target OS.
- Generate AOT-compatible Atto worker entrypoints and native actor lifecycle
  tests, then add true guest `.atto` conformance fixtures and exact SHA CI.
