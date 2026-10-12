# Oreslang process exit status v1

## Portable command-line contract

- A successful zero-argument `pub routine main(): void` returns shell status **0**,
  including an empty body and an implicit fallthrough.
- A successful `main(): int` returns that integer as the shell status only
  when it lies within **0..255**, inclusive. No modulo, masking, truncation,
  or implicit conversion of values outside this range is permitted.
- `std.process.exit(n)` requests status `n` and is a terminal *control effect*,
  not a normal exception to be caught by user `trap` handlers. It is permitted
  only with explicit process-exit authority outside actor-local execution.
  The hosted runtime returns the outcome to its owner; only the standalone
  process launcher calls the operating-system process-exit primitive.
- Missing executable `main()`, invalid source, invalid exit statuses, failed
  initialization, unhandled root-task failures, panics and unrecovered root
  cancellation **must never report success**. The default failure status is **1**.
- Invalid CLI invocation uses status **2**. A successful `--check` exits **0**;
  a failed `--check` exits **1**.
- A root task ending normally may still leave supervised actors running as part
  of an explicit service policy. An actor's *handled* failure is not a process
  failure; unrecovered root/supervisor termination is.
- Explicit host exit occurs after permitted resource cleanup. The earliest
  explicit exit in an unwind remains authoritative; later deferred cleanup
  failures are attached as suppressed diagnostic information, not substituted
  as unrelated process statuses.
- A failure already pending when deferred cleanup begins cannot be converted
  to success by a subsequent `process.exit(0)` (or any other deferred exit).
  A preexisting unhandled failure stays authoritative; later exit attempts
  are suppressed diagnostics. Only an exit requested before cleanup failure
  preserves its requested status.
- External OS signals/faults follow OS rules and are not synthesized as
  language-level statuses. On POSIX, shells can present signal termination
  differently from an ordinary explicit `exit(128+n)`.
- The 0..255 range is an Oreslang **cross-platform portability policy**, not
  a claim that every operating system natively restricts exit codes to 8 bits.

## Backend boundaries

### Java / GraalVM

`LinkedProgramRunner.runWithExitStatus()` returns a host-owned result and
never terminates an embedding JVM. The CLI `OresMain` is the only owner of
`System.exit`. Async root completion is joined before status translation.

### Native LLVM

The generated native host entrypoint is `i32 @main()`; source-level Oreslang
`main` is a separate `i64` (or `void`) guest function. The wrapper compares
the full 64-bit return value against 255 using unsigned comparison and emits
status 1 on negative or oversized return values. This prevents
`return 256` from accidentally becoming a successful shell status 0.

The currently supported native subset does **not** implement source-level
`std.process.exit`, async root tasks or exception effects. Those constructs
must fail compilation rather than silently simulate success. The Wasm32
embedder/WASI entrypoint contract is separate and must not inherit the native
C process wrapper by accident.

## Required regression cases

`void main` empty and explicit return; `int main` values 0, 1, 42, 255,
256, -1; explicitly requested exits; async exit; denied exit capability;
missing executable main; malformed source; invalid command-line arguments;
root failure; cleanup ordering and the first-exit-wins unwind rule.
