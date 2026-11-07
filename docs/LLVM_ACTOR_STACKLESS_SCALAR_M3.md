# LLVM M3: compiler-owned stackless scalar actor call frames

Status: authored **DRAFT**, execution **NOT_EXECUTED** until source-authorized, stepful, exact-commit CI runs. Stacked on LLVM PR #1542, which itself depends on #1536.

## Implemented experimental boundary

The new native-only compiler switch is `--experimental-actor-guest-stackless` (mutually exclusive with `--experimental-actor-guest-frames`). The compiler admits exactly **one synchronous actor**, with 1..16 scalar fields and one `on_message(int): int` plus 1..16 *private, one-argument pure int helper functions* whose bodies contain only a scalar return expression. No source `main`, unverified actor effects, actor handle imports, resource owners, multi-argument calls, class calls, FFI, async, suspension, task, generator, or exception boundaries are admitted. The existing M2 flag and ordinary compilation retain their previous behavior.

The compiler lowers helper calls within a permitted actor-handler expression into an **immutable per-expression instruction array** embedded as a private global in the actual LLVM module. Opcodes include constant/argument, checked signed add/sub/multiply, six integer comparisons, boolean not/xor, conditional branch, helper call, and return. Compiler-side expression budget: 1,024 nodes per bytecode module and 4,096 words; runtime interpreter instruction budget: 200,000 per guest expression.

The generated actor's `on_message` retains its existing M2 authenticated frame enter/read/leave and actor-field i64 heap accesses. For a helper-bearing expression it calls the compiler-owned `oreslang_actor_guest_stackless_i64_execute` rather than recursively calling an LLVM/C++ helper. The M3 emitter deliberately suppresses native LLVM helper definitions entirely. Only compiler-owned bytecode entries represent those helper bodies, so a direct recursive C-stack fallback cannot be linked.

## Independent actor-owned execution frames

The interpreter runs on the real `MultiActorCarrier` scheduler worker. Every call pushes a **120-byte logical frame** on the current actor's `ActorGuestFrameStack`, containing an instruction pointer, operand count, one scalar parameter and 12 operand slots. The interpreter has only one transient decoded frame and 32 opaque stack handles on its physical C stack. CALL saves its parent, then iteratively pushes the next actor-owned frame — it does **not** recurse the native C stack. RETURN pops, scrubs and transfers its result to the parent. Invalid stack/token, overflow, out-of-range branch, non-canonical return, out-of-fuel or stack depth triggers fail-closed process termination. The parent M2 8-byte scalar frame remains live while child logical frames are pushed and popped.

Bounded memory: 32 total logical frames, 4,096-byte store; at most 31 interpreter frames when the M2 message frame occupies depth one. Operand slots are bounded at 12 per frame. This is **not** a dedicated OS/fiber stack or an independent hardware protection domain; actor heap isolation and lexical ownership remain separate obligations.

## Actual guest-execution gate (pending)

`tests/fixtures/actor-guest-stackless-fib-m3.atto` provides three source helper functions (`fib`, `add_one`, `score`) and a real actor message handler. `score` calls `add_one(fib(n))`, with `fib` recursively computing the Fibonacci number using conditional branches. The compiler must produce handler IR that invokes the stackless interpreter with immutable program words, then Clang must link this IR into a native harness with the real actor carrier and actor-owned heap implementation.

The harness creates A/B on one physical carrier with independent actor-incarnation identities and native heaps. Expected effects: A.score(6)=9, B.score(7)=14; STOP A and physically reclaim only A; B.score(8)=22, B.score(9)=35. Both actors must end with empty logical stacks and no protocol failures. A second process sends 48 and **must** abort from the finite actor stack budget before consuming unbounded C-stack frames. A separately compiled modified `.atto` expression that tries to access the mutable actor field as a helper parameter must fail **without emitting any IR**. Non-M3 compilation must never contain the new interpreter symbol.

`tests/native_actor_guest_stackless_m3.sh` compiles genuine source with the actual native LLVM compiler, requires Clang -O0 and -O2 execution, optional Clang ASan/UBSan execution, SHA-256s and unique receipts. The source-owned workflow requires exact head/tree/immutably hashed blobs, nonzero executed steps, and same-run artifacts. If the workflow job fails with **zero steps**, test status is **NOT_EXECUTED**, not a compile result.

## Not certified, no release claim

- No source `spawn` or `ActorRef` using the new frame ABI: a **trusted native adapter** dispatches genuine compiled source actor handlers, not a complete compiled source-main lifecycle.
- No persisted suspended frame across `await`, `select`, `rt cooperate`, or cancellation; the pure expression is synchronous and runs to completion within one carrier turn.
- No general mutability/borrowed local lowering, no dynamic frame shape, no multi-parameter functions or closure captures, no source user exceptions/supervision, no cross-tenant memory sandbox, no hard real-time WCET, no full GC-absence proof for the linked application.
- No evidence for independent physical actor OS stacks; this is an **independent logical stack** per actor.
- No Java/GraalVM parity assertion without identical versioned guest tests on both backends.
- Source export or new cross-organization private CI carriers require independent security authorization.

Next: source-owner authorized runner execution, ORESoftware independent review, source ActorRef/SourceActorBootstrap integration, safe suspension frame serialization and continuation credentials, and genuine Java/LLVM parity receipts.
