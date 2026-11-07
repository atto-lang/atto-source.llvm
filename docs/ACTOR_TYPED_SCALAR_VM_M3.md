# LLVM actor-owned typed scalar call frames — M3 (stacked draft)

**State:** source authored November 7, 2026; no passing hosted exact-head
compiler-backed CI receipt yet. M3 stacks on LLVM M2 PR #1541, itself on
M1 #1537. Do not confuse buildable source or CTest registration with execution
evidence.

## What actually changed

The closed, pure synchronous **unary integer** function graph used as the
*direct result* of a source-compiled actor handler can now be compiled into
bounded interpreter instructions, not invoked recursively through the
carrier's C/LLVM function call stack.

- `ActorScalarFrameLowering.hpp` enumerates and validates **all reachable
  fnc declarations before emitting an instruction**. Up to 16 functions, one
  typed `int` argument each, optional four immutable integer locals,
  pure arithmetic/comparison, one-way conditional early returns, and a
  terminal integer expression/direct tail call. Named/generic/class,
  ownership/borrow, mutable locals, FFI, effects, generators, async,
  actor actions, unsupported statements, or ambiguous unary symbols
  are rejected. No partial fallback to native recursion is allowed for
  any actor-handler expression containing a call.
- `LlvmCodegen.cpp` emits a **constant instruction array, constant function
  entry array, and program descriptor** into the actual .atto guest image.
  The actor's generated native `on_message` invokes only the trusted
  `oreslang_source_actor_scalar_vm_execute` C ABI for this scalar graph.
  Existing function definitions may still have ordinary native LLVM bodies
  for non-actor callers; only this admitted actor path is stackless.
- `SourceActorBootstrap.c` executes a fixed instruction interpreter loop.
  All 64 possible live M3 frames use the existing actor-incarnation-owned
  64-KiB allocation; each owns 48 64-bit words: magic, actor incarnation,
  turn epoch, function ID, PC, stack height, typed argument, 4 tagged locals,
  16 tagged operands. No recursive C function calls implement guest CALL.
  A direct terminal TAIL_CALL wipes/reuses the same actor frame. Ordinary
  CALL uses the next bounded frame, RETURN restores the caller operand.
  Frames and raw values are zeroed on physical handler exit and retirement.
- Each frame is fenced to the current physical owner turn. Static bytecode
  validation rejects invalid opcodes, jump/call targets, bogus function entry
  bounds and quotas **before** guest evaluation. Runtime tag/operand/PC checks
  and a 2,000,000-instruction fuel cap fail closed. Quota or integrity errors
  poison only the current actor; after the physical callback finishes the
  trusted runtime seals that actor's mailbox. A healthy sibling and its
  carrier remain available.
- STOP remains a trusted outer source-actor lifecycle capability. An outer
  stop/join may complete a prior automatic actor fail-stop, but a normal
  double-stop continues to be rejected.

## Test sources and distinct evidence classes

- `tests/source_actor_scalar_vm_runtime_test.c`: host/runtime **executed
  only if CTest runs**. 100,000 tail transfers, a 64-frame non-tail
  overflow, malformed-program preflight failure, sibling activity before
  and after the failure, fresh actor admission after another failed.
- `tests/fixtures/atto-actor-scalar-tail100k.atto`:
  actual compiler-bound tail recursion.
- `tests/fixtures/atto-actor-scalar-mutual-tail.atto`:
  mutually recursive direct terminal transfers.
- `tests/fixtures/atto-actor-scalar-nontail-overflow.atto`:
  100-depth non-tail recursion must be actor-local quota failure.
- `tests/source_actor_e2e.py`: first checks emitted LLVM call instructions
  and negative/no-IR mutants, then links and runs the **real** source actor
  runtime. The default main is allowed to abandon DATA on stop, so it is NOT
  used as a sufficient guest-execution witness. For these three canonical
  M3 programs, a test-only source actor harness links the **unaltered
  generated actor constructor, handler and instruction constants**, renames
  only the ordinary native C `main` wrapper to avoid a duplicate entry,
  and **waits for one actual completed DATA callback** before STOP.
  Only this deterministic completed-delivery path can assert compiler-
  backed guest VM steps/tail counts/overflow receipts. No mock interpreter.
- Existing M2 isolation, ownership, restart and malformed frame-access
  tests stay in the separate native CTest matrix.
- `actor-typed-scalar-guest-native.yml` pins head/tree/blob identities and
  requires three actual runtime CTests, complete compiled source E2E
  including both negative fixtures, and actual compiled .atto execution under
  Clang O1 and ASan/UBSan before publishing evidence.

**A workflow with `steps=[]` or `steps=null` remains runner-admission
failure: `NOT_EXECUTED`. Never label tests passing from authored fixtures,
previous commit heads, source-free canaries or reference interpreters.**

## Conformance vs Java/GraalVM #2640

| Property | LLVM M3 source branch | Java #2640 reference |
| --- | --- | --- |
| Typed PC/function/locals/operands | Actor-owned native scalar words | Actor-owned managed frame bytes |
| Non-tail CALL | Iterative, bounded 64 frames | Iterative, bounded actor frames |
| Tail/mutual-tail CALL | Reuses same native frame | Reuses same actor-owned frame |
| Guest entry scope | Pure unary fnc as direct actor handler return | Wider pure fnc graph in evaluator |
| Nested actor method calls | NOT IMPLEMENTED | NOT VERIFIED full actor lowering |
| Suspension continuation ownership | NOT IMPLEMENTED | Partial earlier SourceTask integration |
| Arbitrary native overflow containment | NOT PROVEN | NOT PROVEN |
| Exact-head executed guest evidence | NOT VERIFIED | NOT VERIFIED |

The semantic convergence is intentionally narrow; **Java/LLVM full actor
stack isolation and parity remain UNPROVEN**.

## P0 risks and future implementation requirements

1. Secure hosted CI admission and real nonzero-step run receipts for both
   M1/M2 predecessors and this M3 exact head. Run native compiler and guest
   under a low carrier native stack as an independent proof.
2. Expand AST proof to multi-parameter functions, typed `bool`, arbitrary
   structured branches, lexical cleanups, methods, actor handler steps, and
   temporary ownership. Do not enable native recursion fallback.
3. Integrate `SourceTask` suspension/continuation metadata into the same
   owned frame memory. Pin actor generation and source task execution across
   cooperative carrier migration and cancellation; require no reentrancy.
4. Establish unified actor heap + frame + mailbox quotas, deterministic
   retirement, and tests with supervisor restarts, timeout cancellation,
   deep mutual recursion, pointer-forgery negative tests and sibling survival.
5. Obtain independent review on exact head; semantic rebase M1 and stacked M2
   when merging into current main. No unauthorized private-source mirroring.
