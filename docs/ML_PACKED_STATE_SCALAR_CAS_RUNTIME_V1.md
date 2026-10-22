# Native actor packed-state scalar CAS: runtime integration slice

## Implemented executable C ABI

`ores_actor_packed_state_compare_publish` is an internal C11 runtime
primitive on the existing single `ActorArena`-backed persistent record.

1. The operation authenticates the current admitted physical actor carrier and
   turn via `ores_actor_arena_read`. Forged domain/epoch and foreign carriers
   fail-stop, consistent with the existing trusted packed-state ABI.
2. It snapshots the entire bounded packed scalar state image into a fixed
   stack buffer: at most 4096 signed 64-bit scalar fields (32 KiB).
3. It requires a mutable initialized version field, nonnegative and
   nonoverflowing expected version, **caller-observed mutation revision**,
   distinct mutable initialized update indices, and exactly one `expected+1`
   version update. Every ordinary field initialization/write and successful
   CAS increments a bounded internal mutation revision. A candidate must
   carry the revision observed with its input weights in the same admitted
   turn: staging after an intervening non-version write cannot accidentally
   authenticate an outdated optimizer candidate. Stale version OR revision
   returns 0 with no mutation. Invalid patches abort before publication.
4. It stages all scalar changes outside persistent storage and makes ONE
   whole-record `ores_actor_arena_write` to publish. The underlying arena
   verifies outstanding loans are absent. There is no allocation, callback,
   kernel or incremental per-field write on the publication path.
5. Existing stable actor-heap ownership remains: the complete scalar image
   is one persistent record, never a collection of cross-actor raw pointers.

## Test coverage

Extends the real native `actor_packed_state_test.cpp` CTest already wired
into the CMake suite (`native_source_actor_packed_state_record`):

- Successful version + weight + optimizer-iteration publication.
- Stale CAS replay and TOCTOU publication with an intervening ordinary
  field write return 0 without modifying the optimizer.
- Three sequential physical carrier migrations with version progression.
- Fork-isolated abort cases for duplicate indices, missing version patch,
  skipped version, immutable field write, foreign carrier invocation, and
  version overflow. Existing initialization/borrow/turn misuse cases retained.
- Shutdown and destruction remain tested on the actual ActorArena.

## Limitations and hard release gates

- This is a **native runtime component**, not currently a compiler-lowered
  Oreslang `handle_state` optimizer transaction. The compiler has not
  proved that all state writes bypassing this API are forbidden.
- The helper does not authenticate gradient senders or require an optimizer
  field to be present. A future compiler-owned typed manifest must define a
  complete weight/optimizer/step/receipt update and bind it to the authorized
  graph edges, sender actor incarnations and nonces.
- One physical actor turn is admitted at a time by this packed-state arena,
  even though three sequential carrier migrations are supported. This
  does **not** establish maxThreads=3 simultaneously executing actor turns.
  The shared actor RW gate and scheduler integration remain separate.
- There is no durable journal, checkpoint recovery, bounded historical
  receipt storage, GPU memory accounting or distributed commit. The
  all-or-nothing in-memory record write is not crash-atomic.
- This PR is stacked on the open native packed-state/current-ref integration
  branch, not based directly on `main` (which currently lacks this ABI).
  Do not promote or merge out of dependency order.

**Evidence:** Native C/C++ files and CTest are committed. Full CMake/CTest,
Clang ASan/UBSan/TSan and exact-head workflow execution have not been
observed for this change. No private source was exported externally.
