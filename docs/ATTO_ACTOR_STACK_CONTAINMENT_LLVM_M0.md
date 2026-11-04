# AttoLang LLVM checked guest-call frames — M0/M1 scalar slice

Companions:
- [Java runtime M0](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2505)
- [Five-backend conformance v1](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/340)
- [Stack-containment tracker](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/341)

## Why this matters

Separate actor-owned heaps do **not** isolate native carrier stacks. Ordinary
compiler-lowered recursive C ABI calls could exhaust a carrier and terminate
AttoVM. Native signal recovery, `longjmp`, or catching an OS stack fault
cannot safely restore the other actors' ownership or heap invariants.

## M0 admission protection

The native LLVM compiler checks same-module source call graphs **before**
tree shaking or LLVM IR emission. It scans all free functions and overload
arity slots, actor handler expressions, class and actor initializers,
conditional arms, lexical locals and deferred call edges, even unreachable
ones. Unresolved targets and graphs exceeding 64 statically nested frames,
4096 functions or 16384 edges remain compile-time failures.

For any recursion involving actors, ownership, defers, region operations,
suspension, host/FFI, exported helper entrypoints, non-scalar params,
generics, or sources without a single qualified OS-main entry, the compiler
still refuses to emit executable recursion. Static host conformance is not
runtime actor isolation.

## M1 bounded scalar-native execution (experimental)

A narrow **pure scalar-expression** function-only module may now contain
direct or mutual recursion. The LLVM emitter validates the same graph again
and inserts:

1. Per-carrier **thread-local** `__atto_guest_depth` and
   `__atto_guest_exhausted` globals. These are guarded execution counters,
   **not physical per-actor stacks or a general virtual-stack allocator**.
2. An entry check before guest execution; the 65th nested source function
   sets an exhaustion flag and returns without incrementing its depth.
3. Immediate failure branches after every lowered source call so that
   multiplication/addition/argument evaluation cannot run after a failed
   recursive call and raise an unrelated LLVM trap.
   **Native stack bytes are not inferred from depth alone:** the scalar M1
   qualification additionally caps parameters at eight per function,
   expression nodes at 128 per function, and total expression nodes at
   4096 per module. These are conservative admission ceilings, **not
   a validated per-target stack-size proof**. A real 1 MiB stack run is
   still mandatory at the intended target/optimization settings.
4. Exactly one depth decrement before each admitted function return,
   including generated failure-return blocks. Denied prologues do not
   decrement. No lexical cleanup/defers/actor turn ownership is admitted.
5. An OS main wrapper that resets the budget at root entry, checks the
   exhaustion flag and maps it to process exit code **75**, not `abort`.
   Ordinary scalar main values still follow the existing 0..255 exit ABI.
6. An out-of-band exported `__atto_guest_frame_exhausted_status()` ABI
   returns the actual compiler-maintained per-thread failure flag (0 or 1).
   Native adapters **must not** infer stack failure from status 75 alone.

The compiler keeps nested recursive helper symbols internal to the module
in this slice, to prevent arbitrary host reentry that bypasses the root
reset. Tail-position source calls currently consume the same checked frame
count; **proper tail-call optimization is not yet proven**.

**Status limitation:** A process exit status is an observable *test
convention*, not a distinct language-level `Result` type or actor failure
cause. An ordinary program could also return 75; only the independent
compiler-generated status query disambiguates those outcomes. Thread-local
counters are not actor-owned and must be released/reset at every admitted
execution boundary. Native actor-generation frame state is still pending.

## Native actor carrier substrate (host only)

The experimental `MultiActorCarrier` recognizes
`Outcome::stack_budget_exhausted` separately from `complete`,
`cooperate`, and malformed outcomes. When a cooperating compiled guest
eventually returns that terminal outcome, the worker retains a
generation-fenced failure marker, does not credit its message, and executes
worker-owned cleanup before retiring the actor.

`native_actor_stack_budget_failure_host_only` models actor A failing and
actor B later completing on the **same** carrier using a host-simulated
budget status.

`native_atto_compiled_guest_on_shared_carrier` goes further on Linux:
the real AttoLang native compiler emits two recursive scalar `.atto`
programs as LLVM IR, Clang links them into two native shared libraries,
and a single `MultiActorCarrier` worker calls both through an explicit
C++ adapter under a 1 MiB pthread stack. A's recursion sets the compiler
failure flag and retires its actor after cleanup; B deliberately returns the
**same ordinary integer 75** with the flag clear and completes on that
same physical worker. This checks out-of-band failure classification and
prevents a spoofed numeric value from being interpreted as a resource
failure. Compiler and image hashes are emitted in the test receipt.

**Limit:** These are actual compiled AttoLang **functions** hosted by C++
actor callbacks, not compiler-generated AttoLang `actor` declarations,
source actor mailbox semantics or supervisor restart. The adapter must
not be promoted as completed actor-language LLVM parity.

## Focused tests to execute

Use a full local LLVM dev build or a funded, independently authorized runner:

```sh
cmake -S . -B build -G Ninja
cmake --build build --target \
  oreslang-llvmc \
  oreslang-native-guest-call-graph-test \
  oreslang-native-actor-stack-budget-model-test
ctest --test-dir build --output-on-failure \
  -R 'native_guest_call_graph_gate_host_only|native_atto_stack_admission_and_shallow_process|native_actor_stack_budget_failure_host_only|native_atto_compiled_guest_on_shared_carrier'
```

- C++ host checker: 64/65 static graph, malformed edges, cycles, arity,
  pure recursive admission vs actor/ownership rejection.
- Compiler-backed `.atto`: shallow return 42, deep direct/mutual/tail
  recursion returning 75 without OS stack fault, **overflow during error
  unwind does not trap**, conditional/unused cycles, arity overloads, and
  denial of exported recursive helpers and nine-parameter recursive frames.
  Execute *both* `-O0` and `-O2` with real Clang. On Linux, repeat
  each budget-exhaustion native process under a 1 MiB stack resource limit.
  No zero-step workflow or single optimization level counts as parity.
- Actor carrier host model: typed terminal reason, exactly-once cleanup,
  same-carrier sibling completion, stale token fencing.
- Require exact head, nonempty GitHub Actions job steps, native compiler and
  executable hashes, observed process exits, and no source export without
  independent authorization. Jobs with `steps=null` are **NOT_EXECUTED**.

## M2 / full parity blockers

- Compiler-managed **actor-generation-owned** virtual frames and cleanup-safe
  failure propagation for methods, ownership, defers, critical sections and
  task continuations
- Proper tail-call optimization where no cleanup/suspension barrier remains
- Actual compiler-lowered `.atto` actor A exhausts, A cleanup runs, B
  responds on the same carrier, supervisor observes and generation-fences
  restart without replaying external side effects
- Low-carrier-stack native process tests, sanitizer runs and hostile hostcall
  tests; arbitrary native/JIT/FFI faults remain in killable OS workers
- Verified stepful receipts on Java, LLVM, Wasm, .NET and Cranelift; host-only
  canaries must never substitute for guest compiler evidence
