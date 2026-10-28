# Native Oreslang `receive` + `await` guest acceptance contract

Status: **canonical source syntax fixed; compiled guest lowering NOT implemented**.
This contract is intentionally distinct from the working synchronous legacy
`pub fnc on_message(int): int` compiler subset, and from the
trusted-`main` `await ActorRef.get_ready()` continuation.

## Canonical guest source

The authoritative source fixture is
`tests/fixtures/source-actor-receive-await-timer-guest.ores`:

```oreslang
define actor Worker as
    receive(message: int) -> void ::
        if message eq 1 then
            await time.sleep(10)
            stdio.out.send("A resumed")
        else
            stdio.out.send("A later message")
        fi
    end
end

define actor Independent as
    receive(message: int) -> void ::
        stdio.out.send("B progressed")
    end
end
```

The syntax contract is **end-delimited**: `define actor`, `receive`,
`if/fi`, `end`, `-> void` and `::`. It does **not** require C-style
braces, C-style `on_message`, or statement-terminating semicolons. Source
identifiers and callable names use snake_case.

## Required compile-to-execution contract

1. **Frontend**: add a proper `ActorReceiveDecl` AST, type-check the
   `message: int` formal and `void` return, parse newline/end-delimited
   statements, conditional `eq`, typed `time.sleep(10)` Future and
   `Channel<string>.send` on `stdio.out`. The limited i64 actor ABI is not
   a legal fallback. The `receive` method is an actor handler and not a
   free function or trusted-`main` callback.
2. **Frame/loans**: lower the suspension point to a bounded actor-owned
   continuation frame with an explicit program counter, original actor
   scheduler domain, actor generation, logical event sequence, code
   generation, captured values and loan pins. No guest pointer crosses into
   scheduler-owned timer/CONTROL entries.
3. **Time**: `await time.sleep(10)` suspends the current logical actor
   event and releases its physical carrier. It must **not** implicitly
   construct a source `Task`. The scheduler clock registers an opaque
   bounded ticket; Future completion publishes READY before the same actor
   event becomes runnable. Use a monotonic clock and an explicit
   cancellation/retirement protocol.
4. **Dispatch**: actor A's suspended message 1 blocks later *ordinary*
   messages for A by default; actor B is independently runnable on another
   carrier. Mandatory supervisor CONTROL may preempt ordering but may not
   execute concurrent actor state. On wake A resumes message 1 before later
   mail; the `else` branch is selected on message 2 only afterward.
5. **Channels**: `stdio.out.send("...")` uses the channel API, not an
   untyped `printf` intrinsic. A guest stdout message emitted by the
   resumed A continuation is observable only after its actual guest entry.
6. **Cancellation**: STOP while A is sleeping revokes the timer and prevents
   a late READY from resurrecting the old event. A claimed continuation
   and its actor heap stay pinned until the last native execution segment
   physically returns and all loans are released. Hot reload must compare
   code generation and actor incarnation before resuming.
7. **Instrumentation**: emit `t_due`, `t_expiry`, `t_ready`,
   `t_guest_start`, `t_guest_end` in one monotonic clock domain. Hard-RT
   admission requires a proved execution-budget bound; deterministic host
   clock fixtures do not establish hard-real-time capability.

## First real guest execution fixture

A test driver should submit A message 1, then A message 2, and submit a
message to B while A sleeps. The exact guest acceptance result is:

- B progresses independently while A is suspended.
- A's logical event 1 resumes exactly once.
- A message 2 does not overtake A event 1.
- Actor A never executes two physical carriers simultaneously.
- Stale/duplicate timer completion does not re-run either branch.
- STOP/hot-reload cancellation cannot revive an old generation.
- No actor memory or continuation pin leaks after physical termination.

An actual compiled `.ores` test must confirm the **generated LLVM IR**
contains the correct receive handler and actor-local resume entry, link
against the real native scheduler and timer/continuation stores, and execute
the compiled guest binary. Old `ready_continuation` tests from the
trusted-`main` slice are insufficient.

## Current negative admission gate

The current C++ frontend only compiles the *legacy synchronous* actor subset.
It now recognizes the exact typed `receive(message: int) -> void ::`
signature and explicitly returns:

`native actor receive -> void requires compiler-lowered actor continuation, timer and stdio channel semantics`

without generating any IR. CTest gates in `CMakeLists.txt` require that
failure, and separately reject a non-void receive and incorrect delimiter.
**These tests are blocking/negative acceptance**, not a successful compile
or a replacement for the real guest acceptance fixture.

The native `ActorExecutionLedger`, `NativeActorLedgerBridge`,
`NativeActorAwaitLedgerLink`, and `NativeContinuationStore` are host
building blocks already exercised by independent focused native tests.
They do **not** automatically change source compiler lowering.

## Release sequence

- Replace the fail-closed `receive` frontend gate with checked typed AST
  construction and control-flow IR for the exact canonical source.
- Connect generated source actor state/continuation frames to the existing
  authenticated actor carrier and timer callback bridge.
- Add actual native LLVM IR/Clang-linked guest execution, including
  sanitizer and cancellation race variants.
- Verify the complete exact source tree and full CTest gates on an
  authorized private CI runner if source-org admission is blocked.
- Extend the *same* canonical guest source contract across Java/GraalVM,
  .NET, WASM, Forge, and Cranelift, with unsupported lowering recorded as
  explicit failure rather than simulated parity.

Keep LLVM PR #1123 **draft** while these guest gates are unimplemented.
