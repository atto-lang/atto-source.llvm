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
