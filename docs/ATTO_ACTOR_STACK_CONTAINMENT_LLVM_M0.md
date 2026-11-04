# AttoLang LLVM actor stack safety — M0 fail-closed admission

Java reference: https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2505
Five-backend contract: https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/340

## Audit result

The native LLVM backend previously emitted ordinary LLVM calls for source
`fnc` calls, and it checked recursion only on the Cortex-M admission path.
A native stack overflow on a shared actor carrier could terminate the process.
Separate actor allocation heaps and guard pages are **not** a recovery boundary.

## M0 implementation (this branch)

`NativeGuestCallGraphGate` conservatively scans all currently representable
same-module direct `name/arity` calls before tree shaking and IR emission,
including conditional arms, lexical/cleanup expressions, actor handlers and
actor/class field initializers. It rejects cycles (including tail-recursive
calls until proven `musttail`/trampoline lowering), unknown edges, more than
64 static synchronous call frames, and unbounded node/edge graphs. A malformed
or unsafe program produces **no LLVM IR**, rather than a native trap/abort.
Host-callable exported functions and disconnected cycles are included.

This is an **admission hardening slice**, not Java runtime feature parity.
In particular it deliberately rejects deep direct/mutual recursion that the
Java guard can execute until its budget is reached. It does not allocate a
per-actor stack, produce actor-local typed `StackBudgetExceeded`, provide
runtime supervisor restart, or prove a low native-stack carrier cannot fail
on a long but acyclic allocation-heavy call chain.

## Evidence and limitations

- `native_guest_call_graph_gate_host_only` verifies the native graph checker
  with direct/mutual/unused cycles, overload-slot identities, actor handlers,
  64/65 depth, edge/node quotas, and malformed input.
- `native_atto_stack_admission_and_shallow_process` invokes **the real**
  `oreslang-llvmc` on `.atto`, verifies that recursion produces no LLVM IR,
  and compiles/executes a shallow `42` example via actual Clang.
- Both tests must be run on an exact head SHA with nonempty runner job steps.
  Authored tests and zero-step Actions are **NOT_EXECUTED** evidence.
- The existing parser and native runtime retain historical `.ores` source
  compatibility; `.atto` is the new public language source suffix.

Next: compiler-managed bounded frames with failure-propagation ABI and
cleanup-safe unwind, proper tail-call optimization for `fnc`/methods, actor
generation and supervisor fences, low-stack native AOT execution, and a
real two-actor process-survival test. Arbitrary host FFI/JIT native faults
remain in a killable OS process/worker; no SIGSEGV recovery on carrier threads.

## Native carrier typed failure M0 (host-only)

The experimental `MultiActorCarrier` now recognizes
`Outcome::stack_budget_exhausted` distinct from `complete`,
`cooperate` and unknown outcomes. The failed actor's generation-fenced
statistics retain the budget exhaustion reason, and its current turn is
not counted complete. The worker runs its existing noexcept cancellation
callback before terminal retirement; a sibling actor can then run on the
same physical carrier.

`native_actor_stack_budget_failure_host_only` tests those transitions
with an explicit host-simulated budget failure. This is NOT a real
compiler-backed `.atto` stack overflow; the LLVM compiler currently
rejects recursive call graphs before generating IR. The callback ABI
remains noexcept: an arbitrary C++ exception, SIGSEGV or native stack
overflow is NOT this recoverable terminal status.
