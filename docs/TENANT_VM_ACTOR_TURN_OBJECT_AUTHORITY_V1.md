# TenantVM actor-owned object memory and physical turn fencing

**Status: trusted native host-only implementation (draft); not an untrusted
machine-code sandbox, actual `.atto` heap lowering, or hard real time.**

This builds on the bounded generation-fenced object descriptors in
[LLVM PR #1892](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1892),
which in turn depends on [#1890](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1890).

## Boundaries introduced

- Existing per-TenantVM `ActorTicket` identifies a live actor incarnation.
  No actor ID string, numeric slot, guest pointer or reconstructed ActorRef
  is sufficient by itself.
- `enter_actor_turn(handle, actor)` admits exactly one physical active
  turn for an actor, on the entering carrier pthread. Its opaque
  `ActorTurnTicket` records tenant generation, actor incarnation,
  monotonically increasing turn serial, thread identity and a separate
  **monotonic per-thread lifetime cookie**. This guards against OS reuse of
  `pthread_t` or `std::thread::id` after a carrier thread exits.
- The root host lock checks every bound object operation synchronously:
  exact TenantVM instance/generation, actor incarnation, active turn
  serial and physical thread lifetime, object descriptor slot/generation,
  actor ownership, and object byte bounds. The same lock guards revoke,
  scrub, descriptor reuse and termination.
- `allocate_actor_object`, `read_actor_object_byte`,
  `write_actor_object_byte`, and `retire_actor_object` are trusted
  supervisor/runtime APIs. They do **not** export native addresses or
  give a guest a C++ object pointer. Legacy host-only object operations
  cannot access an actor-owned object by omitting actor credentials.
- `leave_actor_turn` ends the admission and revokes all copies of that
  turn token. Migrating the token to another pthread, replaying it after
  leaving, or using it during a later turn fails closed.
- Actor retirement refuses live turns, live actor-owned objects, and
  outstanding resource pins. Tenant shutdown still requires all actors,
  objects, native resources and guest invocation pins to drain.

## Quiescent cleanup without new work

`begin_shutdown` closes ordinary turn admission. However, an actor can
still own memory that must be retired. The trusted supervisor may call
`enter_actor_cleanup_turn` only in the Quiescing phase, for an existing
live actor incarnation. Such a turn may **retire** actor objects and leave,
but may not allocate, read or write actor object bytes, and cannot reopen
source guest execution. The cleanup token is bound to the carrier
incarnation just like ordinary turns.

This stage deliberately does NOT introduce implicit asynchronous cleanup.
A lost or abandoned turn can keep shutdown blocked until an external
supervisor determines its physical execution has ended. There is no
forced unsafe memory reclamation and no automatic actor migration.

## Host regression and honest evidence

```sh
for CXX in g++ clang++; do
  "$CXX" -std=c++17 -O2 -DNDEBUG -pthread \
    -Wall -Wextra -Wpedantic -Werror -Isrc/main/cpp \
    tests/native_tenantvm_actor_turn_object_test.cpp \
    -o "/tmp/tenantvm-actor-turn-${CXX}"
  "/tmp/tenantvm-actor-turn-${CXX}"
done
```

A dedicated Actions matrix also proposes Clang/GCC ASan+UBSan. The
regression denies unscoped host access, foreign tenant/actor access,
wrong-thread use of a live turn, nested/replayed turns, bounds failures,
double retirement and stale descriptor reuse. It tests actor retirement,
shutdown cleanup-only authority, tenant recreation and four concurrent
actors on four carrier pthreads.

**Evidence status is NOT_EXECUTED until the exact PR head has real
nonempty job steps and native program output.** A zero-step workflow
failure indicates runner admission trouble, not either a successful or
failing native C++ test.

## Not yet covered

The host controls these API calls but arbitrary native guest code/FFI
can still bypass them using raw process pointers. A physical carrier
thread is not an OS address-space boundary. Do not claim that the
source-level actor compiler is using these APIs, that all LLVM GEP/load
instructions have checked generation provenance, or that MPK/process
isolation has been installed.

Next: bind compiler-generated `.atto` actor object access and the
`ref32` original source + target allocation proof to this turn
authority. Then verify actual native compiler guest IR and O0/O2 native
executions, strict imports/host callbacks, quotas and cancellation.
An independently supervised Wasm/process sandbox remains required for
hostile workloads; use MPK only as optional defense in depth.
