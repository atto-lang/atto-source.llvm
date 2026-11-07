# AttoLang local-storage runtime reconciliation: LLVM drafts #1389 and #1391
2026-11-07 — **proposed integration ledger; NOT executed guest parity**

## One canonical runtime path
Choose `LocalContextDomainsV1.hpp` from #1389 as the **candidate**
compiler-facing host foundation, because its restricted source
`TaskLocal<int>` lowering and executable test harness already reference
it. Do not merge `ScopedLocalStorageV1.hpp` from #1391 alongside it:
it defines competing typed key and actor/task/thread owner abstractions.
Retain the host-only test coverage from both branches until parity is proven.

This is a proposed integration choice, not a review-approved final ABI.

## Preserve each branch's security mechanisms
| Requirement | #1389 (candidate target) | #1391 (source to review/port) | Unresolved acceptance |
|---|---|---|---|
| Typed key / generation | `LogicalLocalKeyV1`, `ExecutionStampV1` | `ScopedLocalKeyV1`, `ScopedLocalIdentityV1` | One compiler-minted static descriptor; neither host record is an authorization capability |
| Actor claim | `ActorLocalSchedulerTurnV1.hpp` | `NativeActorLocalGateV1.hpp` | Scheduler-issued actor ownership + tenant/isolate proof, not copied stamp |
| Task suspension | `SourceTaskRuntime.c` restricted i64 slots | `TaskLocalStoreV1` host model | Real task generation, carrier migration and cancellation retirement |
| Storage quota | 32 slots + newly authored 8 KiB measured payload and strict UTF-8 for strings; NOT deep heap bounds | 32 slots + 8 KiB measured payload | Compare exact semantics, independently execute all quota/atomic replacement cases, reject unbounded guest value kinds |
| Request provenance | `RequestContextHandoffV1.hpp`, `RequestLocalSchedulerTurnV1.hpp` | first-mail sequence in `NativeActorScheduler.c` and `RequestContextLocalV1.hpp` | One-shot original supervisor DATA-mail binding; reject callback, STOP, replay and wakeup substitution |
| Teardown / races | outside-lock destruction, reentrant snapshot coverage | additional request-mail/stop/wake tests | Cancellation and owner-close first revoke logical access, then wait for quiescence and pinned children |
| TLS | physical `thread_local` with teardown fence | physical TLS plus explicit carrier scrub | Do not carry task/actor authority or migrate values across await |

## Integration order and no-regression constraints
1. Extend #1389 native request-mail scheduler with #1391's per-mail monotonic
   sequence admission **semantically**, not by replacing whole scheduler
   source. Preserve current scheduler message/control APIs and queue safety.
2. Port RequestLocal's first-admitted-mail identity fencing and strict
   UTF-8, per-value and total measured payload quotas into the chosen API.
   Ensure `SourceTask` resumed execution is authenticated independently
   of any physical carrier's TLS.
3. Add measured payload quotas for ActorLocal/TaskLocal and physical TLS
   separately; do not confuse `sizeof(T)` for deep allocation accounting.
   Composite guest values require compiler type descriptors, ownership and
   bounds; host-only custom structs remain explicitly non-promoting.
4. Keep all #1389 compiler-backed fixture identities and exit-42 native
   positive cases; add #1391's negative actor/request/mail-sequence/STOP
   and race fixtures as acceptance cases against one runtime.
5. Port only reviewed diffs with green exact-head sanitizer runs. Do NOT
   mechanically overwrite `NativeActorScheduler.c` or
   `RequestContextLocalV1.hpp` between parallel draft branches.
6. Retire the alternate host store only after a compiled .atto fixture,
   native scheduler and independently reviewed receipt cover the combined
   behavior; retain #1391 as draft until then.

## Hard gates
- Compiled `.atto` guest source and linked executable run, with exact
  PR source head and root Git tree; two positive binaries in existing
  `tasklocal_i64_e2e.rs` must exit with 42.
- All 14 negative compiler cases exit **1**, emit no IR/artifacts,
  produce expected semantic diagnostics, and pass new rejection mutations.
- Verify auth-denial for forged actor/task stamps, stale generations,
  wrong tenant/isolate, replayed mail, detached callback, STOP, and
  carrier migration of physical TLS.
- GCC/Clang optimized and ASan/UBSan/TSan target jobs must execute
  nonzero steps; `runner_id=0`, `steps=[]` are admission failure.
- No transfer of private code to another organization without independent
  authorization. Host-only tests and source-free validators are not
  compiler-backed guest parity.

Companion evidence checker: platform-conformance PR #564.
Backend-specific guest implementation remains incomplete and draft.

## November 7 measured quota integration (authored, not hosted-executed)

- `LocalPayloadBudgetV1.hpp` introduces a strict UTF-8 scalar validator and
  a measured-payload function. A scalar or owned string has concrete bytes;
  other host C++ composites are charged only `sizeof(T)` and are **not**
  transitive, owned, or hard-real-time guarantees.
- `LocalContextDomainsV1.hpp` now performs an 8 KiB quota check before
  mutating ActorLocal, TaskLocal, or physical TLS; replacement, erase and
  close update quota atomically for each domain. TLS replacement accounting
  linearizes before a retired value's destructor may reenter.
- `tests/native_context_domains_v1_test.cpp` adds logical/physical
  replacement, overflow, Unicode and stale-after-close regressions.
  `tests/native_local_payload_budget_v1_test.cpp` adds six malformed UTF-8
  sequences and boundary tests. The source-local matrix pins the updated
  helper, runtime and tests to Git blob identity.
- A standalone local GCC C++17 compile/run of the exact payload-helper
  Git blob `ab9741016a8f85e6aef03931a95ecf6d100eeac2` and test Git
  blob `9101f195531c8556a549c8848c6fb05e84a3c919` passed. **The full
  runtime integration and compiled `.atto` guest have not executed** on
  hosted exact-head CI. Maintain draft and do not promote parity.

## 2026-11-07 follow-up: deny shallow STL alias laundering

Native host quota hardening now rejects common dynamic STL container values
(vector, deque, list, forward_list, set/map and unordered collections) before
slot mutation. Nested optional/pair/tuple/variant/fixed-array wrappers are
admitted only for conservative arithmetic/enum representations: they cannot
hide string allocations behind a shallow sizeof(T) charge. Strings alone
continue to use strict Unicode validation and their actual UTF-8 byte length.
This is intentionally fail-closed pending compiler-generated deep-size and
ownership descriptors; custom opaque host C++ objects can still be shallow-
accounted and are **not guest-admissible**.

Added runtime regression asserting rejected vector replacement preserves
existing TaskLocal state, plus seven boundedness static assertions and
seven STL/fixed-size payload policy checks. Updated native-host CI exact blob
pins. An isolated GCC/Clang C++17 compile probe caught an invalid variadic
fold expression during implementation and passed after parenthesization.
The isolated probe is **not** execution of the full GitHub source tree or
combined runtime tests. Current changed header/test blobs and linked guest
are pending stepful execution; retain draft and zero backend-parity claim.


## November 7 P0 scheduler-origin integration — authored, NOT_EXECUTED

The exact #1391 host scheduler provenance implementation was ported to this
draft **without** importing the competing ScopedLocalStorageV1 API:
- \`NativeActorScheduler.c\` blob \`5e2a2cc4522ac14305b11e3c0c3490ac748910a1\`
  mints a nonzero per-actor DATA-mail sequence at successful queue admission,
  carries it in preallocated ring slots, clears physical TLS on exit, and
  associates wake credits with the originating DATA-mail sequence.
- \`NativeActorScheduler.h\` blob \`5f80e91ae45f3f82659abb36f1170a7873c96d53\`
  exposes host-only \`current_data_mail_sequence\`,
  \`current_data_issued_wake\`, and \`is_current_ready_wake\`.
- \`NativeExecutionAuthority.h\` blob
  \`cfd0e7f1d8fbd125a0ca20c7907432fc0407635c\` requires
  exact physical carrier, claim sequence, actor generation and RUNNING
  (not STOPPING) phase to authorize the request-origin path.
- \`RequestContextLocalV1.hpp\` retains #1389's isolated request object,
  owned values, cancellation callbacks and cross-actor handoff compatibility.
  It now adds one-shot \`bind_original_mail_once\` plus 1 KiB per-value and
  8 KiB aggregate **measured** quotas. Strict UTF-8 and known dynamic STL
  denials are reused from the existing single \`LocalPayloadBudgetV1.hpp\`.
- \`NativeRequestDataMailGateV1.hpp\` authenticates the **current** DATA
  sequence at construction, BEFORE binding the first mail, thereby denying
  off-turn/wrong-sequence poisoning. Its getters recheck after copying.
  \`NativeRequestWakeGateV1.hpp\` only allows a scheduler-issued READY
  wake for the exact original DATA-mail reservation; CANCELLED/FAILED,
  stale/replayed tickets or foreign callbacks cannot gain its authority.
- Ported original-mail, two-request DATA, and READY-vs-CANCELLED WAKE native
  tests; corrected the source branch's contradictory assertion that the
  untouched second request must be both empty and nonempty after a wake.
  Added \`native_scoped_locals_45_case_matrix.cpp\`: **45 named host assertions**
  covering actor/task identities, explicit child snapshot copies, physical
  TLS isolation, per-request quotas, malformed Unicode, and teardown.
- New \`native-scoped-locals-mail-reconcile-v1.yml\` pins the Git blobs and
  builds those four binaries for GCC/Clang optimized, ASan/UBSan and TSan.
  The existing RequestLocal workflow's old hash pins were updated and
  TaskLocal compiler/guest workflow now triggers on scheduler changes.

**Unresolved security boundary:** \`RequestLocalSchedulerTurnV1.hpp\` and
\`RequestContextHandoffV1.hpp\` remain legacy HOST-ONLY adapters that do not
yet enforce original-mail provenance for every operation. They MUST NOT be
lowered into the guest ABI or treated as equivalent to the new DATA/READY
gates. Refactoring their APIs and tests requires a separate semantic change.
Per-tenant/isolate authority is host metadata until scheduler-/compiler-issued
tenant and ownership descriptors are proven. A host \`get_copy\` may retain an
already-pinned snapshot through a racing close; only the privileged gate's
post-copy reauthorization is suitable for prospective guest use.

**Gate still open:** These are newly authored source and CI matrices, not
native or guest results. Hosted jobs with \`runner_id=0\` and \`steps=[]\`
remain runner-admission failures. Do not promote drafts, merge #1391, claim
compiler-backed .atto parity, or mirror private source before independent
review, explicit source-transfer authorization and real exact-head receipts.

## Follow-up: legacy bridge and handoff provenance (2026-11-07)

The legacy RequestLocal host adapter now denies an unbound request, another
DATA mail on the same pooled actor, WAKE/CONTROL, foreign carriers and
revoked request generations. It verifies the scheduler-minted original mail
ordinal and rechecks authorization after copying a value.

The native scheduler now exposes host-only authenticated current DATA payload
readback. RequestContextHandoffV1 redeem requires the scheduler-delivered
payload to equal the ticket, binds its derived request to that recipient
mail, and rechecks payload/mail/request authority after copying selections.
Seal rechecks source-mail authority after any user copy constructors.
Wrong mail or foreign attempts cannot consume the ticket.

Selection now OWNS copies of its typed key identities rather than retaining
borrowed caller-key pointers. The regression releases a heap-allocated alias
before seal/redeem, so ASan can catch a raw-pointer use-after-free.

Native test fixtures now cover wrong source mail, unauthorized later message,
wrong recipient DATA payload not burning the ticket, expired key aliases,
scope close during copy, cancellation during selection copy and STOP/revoke.
Original-mail tests also cover current payload/foreign/offturn readback.
Both source-local workflows pin updated exact Git blobs.

Unproven: exact-head GCC/Clang/sanitizer and compiled .atto guest results.
The raw C++ request store, legacy host adapters and custom C++ aggregate
values are NOT guest APIs. Genuine tenant/isolate origin and a jointly
linearized STOP/cancel/write/borrow fence are still required. A STOP racing
an already-authorized host mutation has not been proven impossible.
Retain draft, maintain #1391 as the alternate draft, and do not claim parity.

## STOP / cancellation commit and in-flight lease integration (2026-11-07)

- RequestContextLocalV1::set, erase and first-mail binding now use
  TaskCancellation::with_active_authority at the final store mutation.
  Cancellation/retirement winning graph authority first denies the commit;
  a prior commit linearizes before cancellation can be observed. User value
  copies, moves and destruction are outside the authority graph lock.
- To make the graph-locked commit bounded and allocation-free, request open
  reserves all 32 map slots. A new map node is allocated and extracted
  BEFORE graph authority, then inserted without a rehash. Erase extracts a
  node and destroys/deallocates it outside both store and authority locks.
- get_copy now rechecks open + cancellation/retirement after a user-defined
  copy; reentrant close/cancel fails closed rather than releasing a snapshot.
- Each RequestLocal operation holds a noncopyable, scope-owned in-flight
  lease, including during any host copy/move outside the store mutex.
  close() seals new admission but does NOT synchronously block a carrier.
- New host CONTROL-plane RequestLocalStopGroupV1 registers all request
  contexts of one exact actor incarnation, closes them first, calls
  scheduler.stop_sealed to unblock STOP-dependent work, then waits for
  pinned operations to finish and for scheduler.wait_done. It refuses to
  block the same scheduler's carrier.
  Sibling actors are not closed by this group. Every STOP path must be
  routed through this trusted bridge or separately revoke its requests;
  direct native scheduler STOP remains an unproven guest ABI bypass.
- Added 38 distinct named host assertions covering STOP vs value move,
  cancel vs value move, close vs value-copy, STOP vs pinned read, original
  DATA mail identity, quotas, generation scope, sibling actor survival and
  refusal to wait for STOP from another scheduler's callback carrier.
  Real GCC/Clang optimized, ASan/UBSan and TSan workflow targets are
  attached to the original RequestLocal workflow, with exact blob pins.
- Source-level structural audits and isolated GCC/Clang C++17 node-handle
  and lease syntax probes are NOT full repository native execution.
  Hosted Actions with runner_id=0 or null and steps=[] or null remain
  runner-admission failures; compiled .atto parity remains unproven.

Remaining work before guest ABI exposure: enforce issuer-proven tenant,
isolate and request identity; integrate the STOP bridge at every scheduler
termination path; compiler-owned type/borrow admission; review graph mutex
ordering and sanitizer evidence; exact-head compiled .atto receipts and
independent review. Never treat host C++ custom aggregates as guest values.

NativeActorScheduler now offers a host-only global carrier predicate so
CONTROL-only blocking STOP helpers reject callback carriers from **any**
scheduler domain, not just the actor's owning scheduler. The scheduler
source/header blobs are pinned in both affected workflows. A three-case
cross-scheduler negative fixture exercises this guard; it is not yet executed.

## Native scheduler STOP admission and handoff-ticket arbitration (2026-11-07)

NativeActorScheduler now offers a **trusted-host-only** bounded commit
operation under the SAME scheduler mutex used by ordinary stop(),
stop_sealed(), and whole-runtime retire(). It accepts an exact-generation
actor whose phase is READY, RUNNING, or SUSPENDED and denies STOPPING,
TERMINATED, stale/foreign generations, and globally retired schedulers.
An earlier implementation incorrectly required phase RUNNING even for a
newly spawned READY actor; this was corrected before declaring any tests.

RequestContextLocalV1 set/erase/original-mail binding now take an optional
scheduler pointer used by privileged adapters. All privileged DATA-mail,
READY-wake, legacy actor-turn and cross-actor handoff write paths provide
that pointer, so mutations that STOP wins before their final commit are
rejected. Raw two-argument C++ store calls remain HOST ONLY and must NEVER
be lowered to source-level guest operations. The C callback may do ONLY
bounded, allocation-free bookkeeping: no guest code, callbacks, locks,
blocking, suspension, or arbitrary value destruction under scheduler lock.

RequestContextHandoffV1::seal allocates its envelope before entering
the cancellation + scheduler gate. It creates a one-shot ticket only
after winning both cancellation authority and actor lifecycle admission.
This prevents source cancellation or direct STOP from winning before a
fresh ticket is published. A deterministic test stops actor A during
selected-value copy and asserts no ticket is created.

RequestLocalStopGroupV1 adds explicit finish_after_external_stop() for
trusted host CONTROL repair after direct STOP or global retirement.
The cleanup path checks a new exact-generation is_stopping predicate;
not-ready alone is insufficient evidence because invalid actor handles
are also not-ready. After validation it closes registered stores, waits
for pinned operations and joins actor DONE without blocking carriers.

New native_request_actor_lifecycle_guard_v1_test.cpp has **49 named
host assertions** for live READY commits, both STOP modes, runtime retire,
wrong generation/slot/domain, cancellation, late precommit, and explicit
cleanup of direct STOP. Existing 38 host STOP/cancel tests plus 45 scoped
local host checks remain. The RequestLocal CI matrix now executes the
new cases and an additional STOP-during-handoff-copy fixture. All sources
are SHA-pinned to their Git blob identities.

SECURITY HOLD: direct scheduler STOP/retire does not automatically call
the host RequestLocalStopGroup close/quiescence method. The guarded
privileged write path denies commits after STOP; raw host request stores
may remain allocated until explicit control-plane reconciliation. This
must be integrated with supervisor/panic/restart/hot-reload ownership and
all scheduling teardown paths before claiming universal heap reclamation.
Tenant/isolate provenance, guest-safe owned value descriptors, full native
sanitizer executions and compiled .atto parity also remain open.
Zero-step hosted workflows are runner-admission failures, NOT passing
or failing native/guest evidence.

## Stacked P0 lifecycle observer follow-up (source-authored, not executed)
The scheduler provides a bootstrap-only bounded atomic termination observer
for direct STOP, sealed STOP, and runtime-wide retirement. It is invoked
under the lifecycle lock and cannot perform request destruction, wait,
allocation, external locking, scheduler reentry, or guest calls.
RequestLocalStopGroup rejects registration once the scheduler reports the
actor stopped and reuses the main branch's `finish_after_external_stop`
CONTROL-only reconciliation (no competing second sweep API).
Thirty STOP/sealed/retire two-thread trial rounds and twenty STOP/cancellation
trial rounds plus fixed negative/adversarial assertions are attached as
host-only tests. These tests and the five native/sanitizer workflow jobs are
**authored, not executed**. Actual scheduler-owned request-registry hookup,
guaranteed CONTROL sweeps, panic/restart/hot-reload lifecycle proof, and
compiled `.atto` receipts remain unverified blockers.

## P0 bounded automatic RequestLocal termination registry (stacked draft)

**Status:** native host code authored; source GitHub Actions and compiled
`.atto` execution are **NOT VERIFIED**. This is not a guest-accessible API.

New `RequestLocalActorTerminationRegistryV1.hpp` builds on the bootstrap
scheduler termination observer introduced in LLVM #1525. The registry MUST
be installed before the first actor spawn, with the scheduler's exact
actor-slot count (currently bounded to 256). It owns preallocated slots,
at most 32 strong RequestLocal registrations per actor slot, and one
non-resettable atomic lifetime token per slot. The native scheduler
currently disallows reincarnation in one slot; registry reuse/revival is
explicitly prohibited. The registry attaches this lifetime token to an
individual request *before* its scheduler-gated admission and uses the
same `ores_native_actor_scheduler_commit_if_live_v1` lifecycle mutex to
linearize successful registration against STOP and runtime retirement.

The observer on **ordinary STOP**, **sealed STOP**, or **whole-runtime
retirement** only writes preallocated atomics: it does not lock a RequestLocal
scope or registry, allocate, close request values, reenter the scheduler,
run user callbacks, or block. The token immediately fences future
`RequestContextLocalV1::active`, original-mail binding, `begin_operation`,
`set`, `get_copy`, and `erase` admission for registered requests,
including the post-copy read recheck. A preexisting operation that wins
before STOP remains accounted until its in-flight lease finishes.

A dedicated external C++ CONTROL worker periodically observes invalid
tokens and moves registered strong references out under a short registry
mutex. It closes every revoked scope first and then polls each actor's lease
quiescence and DONE independently **outside** scheduler/carrier/registry
locks. A pinned operation on actor A cannot prevent processing actor B. The
worker polls at a provisional 1 ms cadence and is **not a real-time
latency guarantee**. Production runtime ownership must keep the registry
alive until `shutdown_after_retire()` joins the worker, then drain and
destroy the scheduler, and only then destroy the registry.

The new `native_request_actor_termination_registry_v1_test.cpp` authors
57 fixed checks plus 60 two-thread registration/STOP and
cancellation/sealed-STOP trials and a final count assertion (118 expected
named checks); it also checks sibling survival, generation ABA denial,
per-actor 32-context registration quota, double STOP, direct STOP,
retirement without explicit group reconciliation, cancellation first,
post-stop zero quota, and carrier wait refusal. Three additional negative tests ensure that registry-bound host
mutations cannot bypass the scheduler lifecycle gate by omitting the
scheduler parameter: raw set, erase, and first-mail binding all reject.
The RequestLocal workflow
now pins the new request header, registry and test blobs and compiles the
test on GCC/Clang optimized, ASan/UBSan, and GCC TSan. These are
**source-authored**, not executed receipts.

**Remaining release blockers:** runtime HTTP ingress and supervisor
ownership must instantiate and use this registry exclusively; callers
may still create *raw host-only, unregistered* RequestLocal objects that
are deliberately not promised automatic cleanup. Tenant/isolate issuer
authentication, compiler-owned AOT descriptors, borrow/transitive memory
bounds, task/actor ABA across actual slot reuse, panic/restart/hot-reload
paths, bounded shutdown time, host native sanitizer and compiler-backed
`.atto` execution all remain open. Never equate the regression's
simulated host values with guest-visible or five-backend parity.

### Nonblocking sibling-cleanup hardening

The first CONTROL worker design synchronously waited for each actor's pinned
request operations and DONE before visiting the next actor. This allowed a
single stalled reader to starve cleanup for an unrelated actor. The worker
now first closes every observed revoked request, then uses nonblocking
`closed_quiescent_now()` and `ores_native_actor_scheduler_poll_done()`
per actor; it never calls `wait_closed_quiescent()` or `wait_done()` in
the sweeping loop. A new eight-check regression deliberately parks actor
A's request-value copy while STOP also terminates actor B, verifies B's
context closes and reaps before A's pinned lease releases, then releases A
and verifies its late copy cannot escape. Combined suite: 118 authored
named host checks (57 fixed, 60 coordinated races, final count check).

**Remaining limitation:** C++ host value destructors in `close()` run
outside locks but could themselves block; neither this single worker nor
the shallow host value policy proves bounded cleanup latency under an
arbitrarily blocking destructor. The compiler-facing value/ownership
descriptor, bounded cleanup policy, and integration through the real
supervisor/HTTP lifecycle are still mandatory.


## P0 trusted ingress / supervisor STOP entrypoint — stacked follow-up

**Status:** host source authored and CI workflow expanded. No admitted native
runner, sanitizer pass, or compiled `.atto` proof. Do not merge as guest parity.

The request registry's `create` bootstrap checks its advertised slot count
against the scheduler's immutable capacity, obtained via the new C host
`ores_native_actor_scheduler_actor_capacity_v1` readback. An undersized
observer array no longer silently permits a real actor to escape the fence.

`admit_from_trusted_ingress` constructs a provisional RequestLocal context
and returns it only if scheduler-serialized registration succeeds. On rejection,
the provisional context closes immediately; no unregistered scope is published
by this entrypoint. The trusted transport must issue tenant/isolate/request
identity from an authenticated runtime source: accepting a host C++ struct is
not itself a proof of issuer authenticity, so the API remains HOST-ONLY.

`signal_supervisor_termination` accepts explicit normal STOP, cancellation,
panic, supervisor shutdown, restart, and hot-reload **signals**. Normal STOP
uses the ordinary scheduler STOP; the other causes use sealed STOP so queued
DATA is abandoned. Every path triggers the existing scheduler-owned atomic
RequestLocal lifetime notification. This does **not** implement source
supervisor restart, dynamic actor slot reuse, successful hot-upgrade state
migration, or actor resurrection; those remain independent release gates.

The new `native_request_ingress_supervisor_v1_test.cpp` authors **207**
named assertions (38 fixed admission/bootstrap cases, 48 checks across
the six stop causes, 120 two-thread race checks spanning 60 schedules,
and one final count assertion). It covers wrong tenant/isolate/request and
actor generations, cross-domain/wrong-slot actor IDs, null/cancelled task
authorities, no raw registered mutation, duplicate/late STOP, sibling survival,
disallowed reuse, and concurrent ingress versus each STOP cause.

The source-local RequestLocal workflow adds GCC/Clang optimized, ASan/UBSan,
and TSan builds for the ingress test and **four executable negative
mutations**: remove capacity equivalence, skip registry admission, suppress
supervisor STOP delivery, or remove the registry-bound raw-write gate.
Each mutated binary must **compile successfully**, then fail the exact
expected named assertion; any unexpected failure or a surviving mutation
fails the CI job. Both affected workflow scheduler blob pins were updated.

### Outstanding proof obligations
- Find and update the actual native transport/HTTP ingress call sites and
  actor supervisor dispatchers to exclusively use this trusted entrypoint;
  raw `RequestContextLocalV1::open` remains host-only and must never lower
  directly into guest code.
- Bind tenant/isolate/request identity to scheduler-issued issuer evidence
  and compiler-owned AOT descriptors, not guest-provided integers.
- Prove task cancellation propagation, true actor reincarnation/hot-upgrade
  generation fencing, callback quarantine, ownership/borrow safety and
  supervisor restart, rather than treating a sealed STOP signal as parity.
- Bound or quarantine blocking C++ finalizers: the current CONTROL worker
  avoids blocking on leases/DONE but a host value destructor may itself block.
  Zero-step CI is an admission failure, not evidence for or against semantics.
- Require independently reviewed, stepful exact-head GCC/Clang/sanitizer and
  compiled `.atto` guest receipts before ready/merge or five-backend parity.


## P0 scheduler-minted DATA-turn request issuer (stacked follow-up)

**Status:** host-authored native integration with real scheduler callback
entrypoint; NOT compiled/hosted/sanitizer verified and NOT connected to a
network-facing HTTP parser or AttoLang guest lowering.

`NativeRequestIngressIssuerV1` is an explicitly trusted embedding-host
adapter. Its constructor validates nonzero configured tenant/isolate IDs,
the exact scheduler pointer already bound to the RequestLocal actor-lifetime
registry, and real scheduler capacity. The issuer can only mint a request
while the physical scheduler reports a current actor DATA turn and a
nonzero, scheduler-assigned original DATA-mail sequence. It records
monotonic per-slot DATA-mail issuance and globally monotonic request IDs.
A failed or cancelled admission still burns that mail receipt, preventing
replay by the same actor turn. It constructs `RequestIdentityV1` from
host-provisioned tenant/isolate and the scheduler's actor identity, calls
the mandatory registry's `admit_from_trusted_ingress`, and returns only a
`NativeRequestDataMailGateV1` after revalidating and binding the exact
mail receipt. Failed gate construction closes the provisional request.
The guest never receives raw RequestContextLocal handles from this path.

`native_request_issued_data_ingress_v1_test.cpp` uses real scheduler
DATA callbacks for two independent actors. Across 16 DATA turns each it
authors 240 named checks: 32 scheduler-issued request admissions; exact
source actor/mail authorization and 64 duplicate/cross-actor denials;
wrong/off-carrier actor and issuer rejection; issuer counter monotonicity;
cancelled root authority refusal on actual DATA execution; and actor A
STOP followed by independent survival and eventual STOP of actor B.

`native_request_issued_data_stop_race_v1_test.cpp` authors 391 more
named checks across 60 actual actor callback versus external STOP races:
30 with the DATA callback paused before issuing a scope, and 30 paused
after a valid issuance. STOP must not wait for the blocked callback;
late scope issuance and access after STOP must be denied; the sibling
actor must remain serviceable and the exact terminated actor eventually
reap. Artificially parked carrier callbacks occur **only in the test**
to force a deterministic order; production must not block carriers.

The existing native scoped-mail workflow now compiles these two test
executables across GCC/Clang optimized, GCC/Clang ASan+UBSan, and GCC
TSan, while retaining prior 45-case scoped-mail and original-mail tests.
It also compiles two deliberate negative mutations: issuer-replay
protection and a second-issuer bypass (which would allow independent
per-mail replay counters). Both mutant binaries must build successfully,
then fail their exact named assertions. The workflow additionally compiles
one deliberate issuer-replay mutation, then requires the
exact named duplicate-mail denial to fail at runtime. A compiler error,
unrelated assertion, empty test log, or arbitrary crash does NOT count as
a successful mutation witness. The legacy scoped-mail workflow's stale
`RequestContextLocalV1.hpp` blob pin was corrected; the RequestLocal
matrix's registry pin was updated to the new exact source hash.

**Remaining production blockers:** The repo's native DATA-mail adapter
is now exercised, but no actual application HTTP listener/router or
compiler-emitted `.atto` call has been proven to use it exclusively.
Host configuration of tenant/isolate IDs is not network-principal
authentication, and the opaque C++ API is not a cryptographic bearer
capability. Real supervisor/actor generation restart, hot-upgrade,
side-effect and cancellation receipts, bounded nonblocking destruction,
five-backend guest parity, and independently reviewed exact-head hosted
GCC/Clang/TSan/ASan results remain required. This feature must stay
source-host-only until those gates pass.

**Additional issuer exclusivity:** The registry now has a non-resettable
atomic `claim_data_ingress_issuer_once()` gate. An issuer may claim a
scheduler-bound registry only once for its entire lifetime, closing the
otherwise overlooked hole where two C++ host issuers could maintain
independent per-slot DATA-mail monotonic counters. A failed bootstrap
claim stays sealed; a hot upgrade needs fresh registry/scheduler authority,
not a newly minted issuer for a retired incarnation. One additional named
check (240 issuer checks total) and a second compile-and-execute mutation
witness guard the exclusive claim.

