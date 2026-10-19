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
`snapshot_column()` still creates an uncharged detached vector, so it is
not a guest-safe allocation API: P0 snapshot ownership/accounting in Java
issue #1531 is required before exposing it in source lowering.
Pointers never leave this host API. An actor's logical heap/region
identity is NOT tied to the pthread selected for its next mailbox turn.

In addition to the fixed 16 MiB per-shard limit, constructors now require
distinct trusted host `ActorShardBudget` ledgers for the actor and its VM.
Both are atomically reserved *before* allocation, rolled back on failure,
and released after zeroing the stored scalars in the shard destructor.
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
