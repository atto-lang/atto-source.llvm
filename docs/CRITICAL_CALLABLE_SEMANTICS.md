# `critical`: callable exclusion and actor scheduling

Status (October 10, 2026): Java feature branch has executable file/instance
critical gates; LLVM has a native FIFO ticket substrate but **must fail closed**
on source critical declarations until its actor/compiler lowering exists. Do not
claim language parity from passing the native substrate tests.

## Surface syntax and ownership

```ores
define actor Foo as
    suspend critical critical_section(): int {
        // The logical invocation owns Foo's instance gate until return/failure.
        return 1;
    }
    receive(ActorMail<String> mail): void {
        const value = self.critical_section();
        return;
    }
end

pub critical fnc file_counter(): int {
    return 1;
}
```

- `critical` on an instance member uses one gate **per object** shared among
  all critical members of that object; ordinary members are not locked.
- `critical` on a file/module function uses one gate for the code unit
  within the execution context, shared across overloads and call sites.
  It is not a JVM-global lock crossing separate tenants or isolated VMs.
- `critical static fnc` uses one gate per declaring class.
- `suspend` and `suspends` are equivalent suspension-effect spellings.
- A critical ticket is **logical**, not tied to a carrier thread; actor
  continuation migration must never release or reacquire exclusion.
- Queue is FIFO, bounded to 8192 waiting invocations. Overflow fails closed.
- On normal return, exception, cancellation, and actor/task teardown, the
  runtime releases the invocation ticket and wakes the next waiter.

## Blocking and notification protocol (capacity-one logical queue)

A `critical` gate is an *execution permit*, not a payload channel. Do not
implement it by performing guest-visible `readch` / `writech` operations:
those transport values, obey close/select semantics, and may share ordinary
mailbox backpressure with unrelated work. Reuse the scheduler's Future/await
machinery but retain a separate internal admission queue.

1. `request` first checks the gate under a **short host mutex** (queue metadata
   only). If idle, assign the caller the permit immediately.
2. Otherwise append a distinct, cancellation-aware ticket to a bounded FIFO
   queue; park the **logical continuation** in the scheduler's WAITING state.
   Never occupy a worker/carrier thread to wait on a permit.
3. On return, throw, panic, cancellation or teardown, release the invocation's
   token exactly once. Under the short mutex, atomically transfer the permit
   to the oldest non-cancelled ticket, then unlock.
4. Publish the admitted ticket to the scheduler's **internal control/resume
   queue**, not the actor's ordinary user-message mailbox. This must not be
   starved by bulk regular mail. The scheduler, not `release`, later runs the
   continuation on an admitted execution turn.
5. On resume, verify the ticket still owns the gate (cancellation may have won
   after handoff). Never run guest code directly from a Future completion or
   while holding the host mutex.
6. Waiting-ticket cancellation removes the ticket from FIFO. If cancellation
   races with ownership handoff, it must release the newly acquired permit and
   wake the next eligible caller. Capacity excludes the single owner; 8192
   waiting tickets are permitted.
7. Host/embedder *synchronous* callers may use a targeted blocking signal.
   Synchronous **actor** and scheduler callers must never park a carrier;
   without compiler-lowered suspension, contention fails closed with a
   diagnostic instead of blocking native workers.

The Java runtime uses a per-ticket `OresFuture` for asynchronous admission and
an individual `CountDownLatch` only for a host-side synchronous fallback.
The LLVM native gate returns an opaque gate-scoped token and an admission
status; `release(token)` returns the newly permitted token, which its
scheduler must enqueue. The native substrate currently does not include source
actor continuation lowering.

## Preemption contract

1. Ordinary non-suspending callables execute to completion on their admitted
   execution turn. The runtime can *observe* cancellation/fuel at a safepoint;
   it cannot arbitrarily unwind or transplant a Java/C++ stack and resume it.
   Host OS preemption may pause a physical thread but **does not admit a second
   critical invocation**.
2. `suspend` / `async` callables may yield at compiler-lowered `await`,
   `rt cooperate`, blocking-channel and explicitly resumable safepoints. A
   yielded critical callable keeps its ticket.
3. Compiler-inserted loop cooperation is suppressed while a critical ticket
   is held. A future policy to permit it must retain the critical ticket
   across the handoff and prioritize the continuation consistently.
4. An ordinary actor turn remains single-flight; `requestConcurrency` and
   `messageConcurrency` are separate limits. A critical method can narrow
   mutual exclusion even on a concurrently admitted actor.
5. Never acquire a critical gate by parking a carrier while in an actor or
   scheduler task. As a guardrail, a contended *synchronous* callable currently
   fails with a diagnostic recommending `suspend critical`. Full seamless
   suspension of synchronous callers needs compiler-generated callsite
   continuations and effect typing, not a JVM thread lock.
6. Recursive calls and cross-gate cycles need clear deadlock handling. Code
   must not await another invocation of its own critical gate while holding it.
   A full wait-for-graph/cycle detector remains a release gate.
7. A synchronous critical function calling another suspending function must
   follow the compiler's existing suspension-effect checks, not silently
   turn into an implicitly asynchronous function.

## Implementation boundaries and release checks

For Java, review parser metadata, type/ownership admission, evaluator dispatch,
logical FIFO gate, actor cancellation, and multi-actor tests under contention.
Check tail-call optimization does not release a critical ticket before the
logical call returns. Any guest-visible critical call that is not provably
protected must reject at compile time or fail closed.

For LLVM, native CriticalGate only establishes bounded logical-token
admission/handoff. The frontend currently accepts a narrow i64-only language
subset and rejects actor and async lowering. Compiler lowering, actor class
allocation, source task continuations, cleanup/unwind integration, and
cross-thread stress testing are still required before enabling `critical`
declarations. Use the exact-source-tree funded CI mirror when original GitHub
Actions cannot allocate a runner.
