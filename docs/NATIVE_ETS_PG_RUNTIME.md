# Native ETS / pg and VM bootstrap parity (host-only P0/P1 foundation)

Status: **review-only draft**. Tracks native shared-state umbrella [#659](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/659). Do not confuse bounded host primitives with compiling Oreslang ETS/pg, a secure IsolateVM, or Erlang/OTP distributed functionality.

## Normative ownership topology

```text
process main() (existing host thread; no extra bootstrap master)
  OresVM [CONTROL lifetime authority, global budgets, feature gates]
    IsolateVM [tenant memory, table/actor ownership, actual security boundary policy]
      ActorGroup [supervisor-owned lifecycle + separate cooperative memberships]
        Actor / Mailman [logical tasks, CONTROL priority, bounded DATA]
    shared OresScheduler [MAIN, SHARED, TRUSTED_PRIVATE, UNTRUSTED quotas]
    NIO reactors [epoll on Linux; readiness records only]
    ETS table registry [isolate-scoped tables, typed generation handles]
    pg group registry [isolate-scoped membership, snapshot+mailbox fanout]
```

Thread topology:
- Throughput cold start: 1 already-running host main thread; **0 newly spawned OresVM master, ActorGroup, actor, Mailman, scheduler-carrier, completion, and NIO reactor threads**. Creating metadata/FDs is not equivalent to spawning a thread.
- First independently runnable actor turn: a bounded lazy carrier in its correct domain; a mailbox backlog on one serialized actor cannot start additional carriers by itself.
- First valid socket readiness watch: lazy reactor #0 (up to 4 by verified load); reactor never executes guest code or Future subscribers. Completion goes to the owning logical scheduler.
- A VM-wide cap must account for cross-domain carrier permits with a reserved CONTROL progress permit. Existing four-domain default maxima are **not** yet a proven work-conserving global budget.
- No implicit per-actor, per-ActorGroup, per-isolate, per-table, per-Mailman or per-connection threads.
- Explicit fixed user scheduler N or certified RT profile are exceptions: prestarted bounded threads and reactors are configured before guest admission. Zero tracing GC is not alone a WCET or hard-RT guarantee.
- UNTRUSTED is currently an in-process callback lane, **not** a tenant sandbox. Do not execute hostile native code there without an independently validated OS-process boundary.

## ETS-style tables: initial host proof

`NativeEtsScalarSet.hpp` is a fixed-capacity, scalar `uint64_t` key/value **set**, with:
- Isolate/runtime/owner-generation identity, nonwrapping per-table incarnations, access modes `private`, `protected`, `public`.
- Insert/insert-new, lookup (status separate from value), erase, bounded checked increment, capacity errors and owner-retirement invalidation.
- Preallocated fixed table slots; per-key atomic methods under a table mutex; no tracing GC. No allocation on simple set operations, but mutex contention **can park** and scans are O(capacity), so this is **not a hard-real-time data structure**.
- **Critical trust boundary:** the `Principal` parameter is asserted by the trusted host, not authenticated inside this class; guest access requires an unforgeable, runtime-derived principal and a live actor-generation check at each operation. Numeric caller IDs are not security capabilities.
- Owner-linked cleanup is host-driven and must be wired into physical-finalization paths. No heir transfer, named tables, ordered_set/bag, shard hashing, typed payload serialization, actor buffer moves, iterator safety, table registry global quota, guest API, native codegen or multi-tenant IPC is claimed.

Erlang ETS's `protected` mode permits non-owner reads and owner writes. `public` permits other principals to write **within the table's authorized isolation boundary**, not across arbitrary tenants. Actual Oreslang tables must not alias actor-private mutable heaps; mutable shared state must be placed in a dedicated bounded shared region with validated loans.

## pg local group snapshots: initial host proof

`NativeActorMembershipRegistry::snapshot_group` returns a group-capacity-bounded, runtime-scoped, incarnation-tagged snapshot of `(actor_id, generation)` members. A missing group returns no snapshot. Copy happens under the registry lock; **all sends occur later, outside that lock**. An actor can belong to several groups; no snapshot grants stop/finalization ownership.

Erlang `pg` provides process-group **membership discovery**. A broadcast is built by retrieving members and sending individually; it is not an atomic delivery primitive. For Oreslang, do not claim a linearizable atomic fanout transaction: membership may change after snapshot. Every recipient must still be authenticated as live at its owning VM/isolate mailbox, and bounded send admission may individually fail because of STOP, full inbox, unavailable remote transport, stale generation or authorization revocation.

The additional `snapshot_group_limited` verifies the caller's maximum recipient count **under the group mutex, before allocating a snapshot**, so an over-budget request performs no member copy or delivery. `NativePgBroadcast.hpp` implements a trusted-host local best-effort broadcast adapter with bounded recipients, per-recipient `accepted/rejected/stale` results, a no-throw delivery callback outside the registry lock, and fresh membership checks against stable `group_incarnation`. Rotating join permits leaves that incarnation unchanged; closing/recreating a group allocates a different incarnation to reject old snapshots. Revalidation is not a replacement for the mailbox's own generation/permission/STOP admission checks; a close can still win after the revalidation and before the callback.

This is not a cluster `pg` service: missing are per-isolate subscription authorization, checked `ActorRef` conversions, source `pg` APIs, clustered discovery/partition semantics, authenticated cross-process transport, distributed dedup/replay control, causal ordering and per-recipient backpressure/receipts.

## Promotion gates

1. **Exact-head native host checks:** strict GCC+Clang C++17, `native_ets_host_scalar_acl_lifetime`, `native_ets_insert_new_and_retire_race`, `native_pg_host_membership_snapshot`, `native_pg_bounded_broadcast_and_group_aba`, and Linux `native_vm_ets_pg_zero_cold_threads` through CTest, sanitizers, concurrent join/leave and increment, owner death/recreate, cross-isolate denial and capacity exhaustion.
2. **VM integration:** owner-death hook on physical retirement, table registry lifecycle and quotas, authenticated generation-qualified handles, bounded audit counters; VM close must reject/reconcile lingering table/pg loans.
3. **Source lowering:** parse/type-check explicit native `ets` and `pg` operations; enforce Sendable/Serializable and ownership; compile actual `.ores` fixtures to LLVM IR and execute AOT; reject unsupported types, table modes and cross-isolate access without emitting runnable IR.
4. **Scheduler/NIO composition:** no callback under registry/table mutex, no actor carrier blocking on I/O or table contention, reserved CONTROL, full STOP/drain cancellation and reactor completion fences.
5. **Cross-backend differential:** same success/failure traces against Java backend for owner death, stale handles, concurrent counters, leave/broadcast races, mailbox full, supervisor panic, restart and isolated tenants.
6. **Security and CI:** source-org stepful exact-head execution or a *separately authorized* reviewed private carrier. The source private-CI allowlists/source export remain a HOLD; runner availability alone never authorizes mirroring. Zero-step Actions results are not execution evidence.

### Local exact-blob execution results

- `NativeEtsScalarSet.hpp` plus `native_ets_linearizable_retirement_test.cpp`: verified Git blob hashes of both local sources match the branch. Both GCC and Clang strict C++17 builds passed, with **8 ASan/UBSan runs per compiler (16/16)**.
- `NativeActorMembershipRegistry.hpp` plus `NativePgBroadcast.hpp` and `native_pg_broadcast_test.cpp`: all three compiled local sources match the branch Git blob hashes. Both GCC and Clang strict C++17 builds passed, with **8 ASan/UBSan runs per compiler (16/16)** after the bounded-preallocation fix.
- These are **local native host tests**, not stepful source-org GitHub Actions. The new Linux VM thread-census test, legacy pg snapshot and complete CMake/CTest + actual compiled `.ores` guest remain unverified. Do not mark promotion ready.
- On these tests `recipient_limit` returns no copied member count by design (0), and zero callback invocations. The old unbounded `snapshot_group` remains a separate trusted host API and must not be exposed as an unbounded guest broadcast entry point.


## UHL-1 owner-death heir handoff (stacked host-only prototype)

NativeEtsScalarSet now supports a **trusted supervisor-originated** `retire_owner_with_heir` -> `acknowledge_heir` (or `abandon_heir`) protocol. From the first owner-retirement operation until exact single-use receipt acknowledgment, every DATA access is denied, including PUBLIC-table readers/writers and the successor. ACK requires a distinct, trusted `confirm_owner_retired` witness **after** the real actor registry, continuation/Task/FFI and ETS lease retirement check; a receipt alone never authorizes commit. This host-side flag does not itself authenticate death and remains untrusted without the OresVM integration. Only then does ACK transfer the authenticated owner Principal and advance a nonrecycling `table_epoch`; `Handle::incarnation` and committed rows remain stable. Failure closes/clears the table rather than resurrecting the dead owner or enabling two writers. Stale old owners cannot regain PUBLIC access; a bounded 64-retirement history fails closed at exhaustion. No callbacks execute while the table lock is held.

**Not yet an ETS-heir feature advertisement:** numeric Principal/receipt fields must never be accepted as guest credentials. The actual OresVM must authenticate actor death, heir liveness, tenant/isolate, supervisor ownership and table lifetime; serialize registration/retirement with its actor registry; enforce source-schema/ACL/type checks; and deliver the receipt only on CONTROL. No compiler-emitted source `otp/ets`, code-image transaction, cross-node ETS, durable table, real supervisor restart policy, or exact-head native guest execution is provided here. Therefore this slice does **not** enable UHL-1 `ETS_HEIR` in the shipping capability probe.

Host test `native_ets_uhl1_heir_handoff_host_only` covers frozen writes/reads during transfer, forged/duplicate/foreign receipt rejection, old-owner revocation, table ID/row preservation, failure closure and bounded sixty-four owner changes. Run GCC and Clang strict warnings plus sanitizers on an authorized runner before marking it verified.
