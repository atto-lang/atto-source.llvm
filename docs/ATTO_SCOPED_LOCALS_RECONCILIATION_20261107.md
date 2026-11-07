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
