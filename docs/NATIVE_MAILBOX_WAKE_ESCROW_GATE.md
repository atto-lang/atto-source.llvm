# Native async mailbox continuation-wake escrow — execution gate

Status: **native scheduler primitive only**. Not yet an implementation of
guest `ActorRef.send_async`, Channel/SelectSet source lowering, or Java/LLVM
behavioral parity.

## Separation of authorities

The scheduler has three independent delivery resources:

* Supervisor CONTROL/PANIC: existing one reserved control message per actor.
  Wake saturation cannot consume its slot or suppress STOP.
* Continuation WAKE: eight preallocated producer-local escrow credits when
  the scheduler is explicitly opened with `open_with_wakes`. No additional
  heap allocation, pthread creation or fallible enqueue at publish time.
* USER DATA: the existing bounded actor mailbox, unchanged.

The scheduler always prefers supervisor CONTROL over WAKE over USER DATA.
All three execute under the same actor-exclusive physical claim. No completion
thread directly calls guest code.

## Exact sequence required of the future mailbox implementation

1. Check validated producer and target generation/capability and structured
   self-send dependency. Do not allow serial self-send to suspend awaiting
   capacity that only its own active turn can release.
2. Acquire the bounded producer descriptor/retained payload byte budget,
   **and reserve one scheduler wake ticket**, before any producer can become
   pending. The whole prepublication cohort, not only visible waiters, is
   bounded (Java currently uses 256 per target). An unrelated producer-domain
   cap is required so one producer cannot register unbounded waits across
   many targets.
3. Freeze/copy and retain payload ownership while charged to the target's
   exact incarnation; register the waiter under the same mailbox-accounting
   arbitration as release of USER capacity. No check-then-sleep window.
4. A single mailbox critical section must atomically claim FIFO USER credit,
   mark the wake ticket **TRANSFERRED**, and make cancellation lose. Keeping
   mailbox USER credit charged during handoff prevents racing synchronous
   senders from stealing it. An abort prior to publication refunds that credit
   and calls `abort_transferred_wake`, never `cancel_wake`.
5. Commit the admitted envelope or settle a terminal failure exactly once.
   `publish_wake` is non-failing on a live authentic transferred ticket.
   A producer STOP which wins before publication returns
   `ORES_NATIVE_WAKE_PRODUCER_RETIRED`; it is not a successful admission.
6. Settle Future/observer callbacks **outside** mailbox, lifecycle and
   scheduler locks. Release physical admission/arena and code-generation loans
   only when target payload bytes and credits have been published/returned.
   An admission ACK is not a processing/drain ACK.

The wake ticket binds the authenticated producer scheduler/actor generation,
target actor generation, continuation slot+generation, code generation,
descriptor generation and a monotonically incremented escrow slot generation.
The target binding is validated locally when the target is in the same
scheduler; cross-scheduler targets need separate verified bridge authority.
The scheduling primitive does not itself validate cross-VM message ownership.

A RESERVED ticket may be cancelled; TRANSFERRED may only publish or abort as a
terminal failure. For a target in the same scheduler, TRANSFER revalidates the
exact live target generation; a target STOP which wins after reservation makes
TRANSFER fail while leaving the producer ticket cancellable. Wake mutation
calls against a scheduler opened without wake support fail closed rather than
touching an absent wake table. READY becomes claimed by a single actor worker;
physical DONE remains fenced while RESERVED, TRANSFERRED, READY or EXECUTING.
At producer STOP, a later transferred wake is terminalized without running
guest code.

### Existing runtime integration work

* LLVM #899 — shared contract only.
* LLVM #913/#922 — native Future continuation readiness/commit transaction.
  Compose READY and scheduling only after reserving wake delivery. Do not
  substitute a generic cancellable READY state for irreversible mailbox credit.
* LLVM #894 — priority mailbox/physical loans; integrate after eliminating
  duplicate CMake target registration in that branch.
* LLVM #539 — earlier independent capability wake, but publication can reject
  on slot saturation; do not rely on it for transferred mailbox admission.
* Java #1978 + #1981 — combine through integration draft #1983. Standalone
  #1981 should not silently inflate physical USER capacity.

## Native stepful execution required

At the **exact integration Git tree**, compile/link the actual scheduler with
strict GCC and Clang; run:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build -R 'native_continuation_wake_(escrow_stop_cancel_aba|carrier_failure_fence)' --output-on-failure
ctest --test-dir build -R 'native_(actor_nonblocking_ready_queue|actor_send_stop_linearizable_race|actor_typed_panic_exclusive_carrier|continuation_ready_cancel_resume_race|priority_user_inbox_control_isolation|actor_resource_loan_terminal_race)' --output-on-failure
```

Also execute full CTest plus GCC/Clang ASan/UBSan and applicable TSan runs
on the integrated tree. The new escrow fixture covers saturated wake credits
alongside CONTROL, single-flight dispatch, ABA replay rejection, paired
cancel/transfer races, physical DONE after STOP, and eventual retirement.
The creation-failure fixture injects actual pthread creation failure and
proves a wake reservation fails **before** credit transfer.

Future gate (not implemented here): deterministic target slot release versus
wait registration, mailbox STOP after transfer, retained payload bytes and
physical loans, producer generation/continuation cancellation, 256-descriptor
target bound, actor-local select quota isolation, network/stream propagation,
guest compiler positive/negative tests, and differential outcome traces.

## Java and GraalVM parity gate

Execute Java integration #1983 at its **exact head**, not two independently
passing PR branches. Run targeted actor async-send races, mailbox/Channel
domain integration, Future listener attachment, output termination, and
actor physical-finalization suites, then offline Maven native-carrier verify
under GraalVM JDK25. Build and actually run the guest AOT/hybrid Native Image
smokes. Compare acceptance, cancellation/STOP winner, FIFO, error classification,
charged bytes and physical DONE across real Java and native guest execution.

## Provenance and private CI

No source-org workflow whose job has no executed steps proves compilation or
runtime conformance. Source-free capacity probes in alternate organizations
prove runners/toolchains only.

The source repositories' private-CI allowlists are empty. Cross-organization
private-tree mirroring (including through the GitHub connector) is prohibited
until the independent reviewed source-side allowlist and carrier contract
authorize the **exact numeric repository ID**, reviewed workflow/base and
immutable test plan. Use the reviewed V3 exact-tree SHA receipt, restricted
source-free preparation, offline/test sandbox, stepful results and cleanup
workflow only after approval. Never export to a public repository.
