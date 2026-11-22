# Native std.process design — LLVM backend

`stdlib/std/process_policy.atto` implements guest-side, allocation-free
preflight arithmetic for child launch shape, stdio, actor budgets, bounded
capture, monotonic deadlines, termination escalation and exit success.
This is **not** an executable spawn implementation or host capability grant.

## Required native implementation

- Expose `await process.spawn(Command)` returning an owned
  `Result<ChildProcess,ProcessError>` via a narrow native runtime ABI.
- On POSIX, prefer `posix_spawn()` with explicit fd actions, close-on-exec,
  bounded argv/env, permission-checked executable/cwd and explicit process
  group ownership. No ambient shell or implicit PATH search. If any fork+exec
  fallback is indispensable, keep the forked child path async-signal-safe and
  exec immediately; never return into the multithreaded Atto runtime.
- `stdin/stdout/stderr` are nonblocking, bounded, backpressured channels.
  Integrate read/write and process-exit watches with the existing actor
  reactor; `wait()` never parks a carrier thread.
- Reap exactly once. Own a child-generation ticket, not just a PID. Avoid
  PID-reuse races. Linux pidfd may optimize but must not be required on
  every native target. Implement platform-appropriate exit observation.
- Owner cancellation or scope teardown closes pipes, requests termination,
  applies bounded escalation and reaps. Detached children require explicit
  authority and supervisor/reaper responsibility.
- Apply RUN permission at the native spawn boundary, independent of guest
  preflight, plus actor-local capability attenuation, command and env scope
  checks, budgets and cwd/file authority.
- Return structured spawn, timeout, cancellation, wait and output errors;
  never conflate an OS launch failure with an ordinary nonzero child exit.
- Keep this I/O-hosted module out of freestanding/embedded/hard-RT profiles
  unless such a target explicitly provides process capability.

## Current evidence

`tests/fixtures/std-process-policy-v1/main.atto` exercises only the
pure .atto policy (expected exit 42). No real guest compiler execution,
native child creation, AOT, async I/O or lifecycle test is yet verified.
Status: **SOURCE ONLY / NOT EXECUTED**.
