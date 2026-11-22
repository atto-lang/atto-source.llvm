# Native actor-owned scalar shards and VM physical-turn gate

Status: **host/runtime C/C++ vertical slice**, stacked on PR #671. This is
not compiler-generated actor or `nb` Task code, not an untrusted sandbox,
and not global NUMA-aware work stealing.

## ActorShardColumns.hpp

One native actor owns a deterministic scalar allocation using the existing
`dop::Plan` address formulas for AoS, SoA and AoSoA (power-of-two tiles).
The host creates a shard only while that exact generation-checked actor
callback is running. Every read/write/integration and copied column
snapshot verifies `ores_native_actor_scheduler_is_current_actor`.
`copy_column_into(field, output, capacity)` copies into existing trusted-host
storage without allocating and rejects undersized buffers. The legacy
`snapshot_column()` signature now fails closed instead of creating an
uncharged detached vector; callers must use the leased snapshot or bounded
copy API. P0 #1531 still gates source lowering.
Pointers never leave this host API. An actor's logical heap/region
identity is NOT tied to the pthread selected for its next mailbox turn.

The new trusted host-only `snapshot_column_leased(field)` returns a
noncopyable RAII `ActorColumnSnapshot`. Its allocation is atomically billed
to both the exact actor-generation budget and VM budget before allocation.
Only the creating shard can populate the snapshot. Read, metadata and
bounds-checked `copy_into` are authorized either from the live owner actor
turn or from a trusted host thread outside scheduler callbacks. A foreign
actor carrier is rejected. Actor retirement revokes reads/copies, while a
trusted external host may still call idempotent `close()` to scrub/free and
refund the outstanding charge. RAII destruction provides the same cleanup
without an active actor turn. The shard destructor has the same
free-before-refund order. A standalone `native_actor_shard_snapshot_quota_and_generation`
CTest exercises 1000 snapshots, generation fences, quota exhaustion and
refunds.

**Host lifetime requirement:** the actor and VM budget ledgers must outlive
all snapshots; snapshots must be closed/destroyed before reclaiming the
native scheduler, since native actor-stop registration/revocation is not
wired to these host-only handles yet. Therefore this is NOT a complete
guest-visible snapshot lifecycle; P0 #1531 stays open. The legacy
`snapshot_column()` signature is retained only to reject old unmetered
callers and no longer allocates a vector.

In addition to the fixed 16 MiB per-shard limit, constructors now require
distinct trusted host `ActorShardBudget` ledgers for the actor and its VM.
Both are reserved under a paired ledger lock *before* allocation, so the
actor and VM charges commit at one linearization point with actor retirement.
Allocation failure refunds the pair together; destruction releases the pair
after zeroing/freeing the stored scalars.
The host must keep both budgets alive longer than every associated shard
and must pass the actor's correct budget; the ledgers themselves grant no
access to data. This is a host-accounting substrate, **not** a verified
compiler-generated actor arena or zero-copy borrow model.

Strict warning CTest:
`native_actor_owned_aos_soa_aosoa_dense_kernel`. Manual benchmark:
`oreslang-native-actor-shard-bench`, using 1K/10K/100K rows with each
physical layout, without performance thresholds.

## Shared VM physical turn gate

The new trusted `ores_native_vm_open_with_physical_turn_limit(config, N)`
enforces at most N concurrent actor callbacks across the four scheduler
domains. `ores_native_vm_open_cpu_budgeted(config)` now also installs
a gate with maximum equal to the discovered effective processor count.
Legacy `ores_native_vm_open` retains its original scheduling behavior.

Permits are acquired before an actor callback becomes active, and released
only after it has finished its actor execution claim. If no permit exists,
the scheduler leaves the message on its original generation-checked queue
and its carrier idles on the scheduler condition variable. A rejected
logical actor does not block a user Task, invent a fifth worker pool, or
change actor data ownership. Permit releases notify the scheduler domains
only while denied work is registered. The second-chance permit check
prevents missed wakeups. Wakeups also remain valid during retirement,
so waiting accepted mail can drain. All four schedulers are fully drained
before any is destroyed to avoid an obsolete gate wake using freed memory.
The gate is opaque in the public C/C++ actor scheduler header.

New CTest: `native_vm_cross_domain_physical_turn_limit` validates N=1,
cross-domain admission and teardown with a pending actor. Existing native
actor and CONTROL tests remain required.

## Remaining limitations

Physical permits are VM-local, cover actor callbacks but not arbitrary
independent native/guest Tasks, and do not prove hard OS CPU pinning.
Pthreads may idle/park while no work may be admitted; *logical actor
execution* never parks on a permit. No NUMA/topology discovery or true
per-core shard placement exists yet. Host-owned `ActorShardColumns`
uses copy-out snapshots, not inter-actor zero-copy mutable borrowing.
Exact-head CMake/CTest, sanitizers, and executable source conformance
are merge gates; CI workflows queued or failing before steps are not evidence.

## Required native actor-stop budget fence (host API)

After sending stop/retire to the actor, invoke
`retire_actor_shard_budget_after_done(scheduler, exact_ref, actor_budget)`
from a trusted non-carrier host thread **while the scheduler still exists**.
The helper waits for the exact generation to finish and marks the actor
quota retired. New reservations and all actor-owned shard/snapshot data
access are then rejected before any scheduler call. Outstanding snapshots
remain charged until their RAII destruction.

Before deleting the scheduler or actor/VM budget objects, destroy all
shards and snapshots that reference them. This remains an explicit host
lifetime contract, **not automatically wired into every native VM shutdown
or guest code path**. A host which ignores it may still create dangling
lifetime references. See P0 #1531.
