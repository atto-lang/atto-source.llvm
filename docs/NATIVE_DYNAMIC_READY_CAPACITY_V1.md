# Dynamic native actor ready index — bounded allocation contract

Draft implementation stacked on native indexed-ready #676 (itself stacked on pthread locality #672). This is native *host-runtime* metadata; Oreslang source lowering is not yet enabled.

The two CONTROL/DATA ready bitmaps are separately allocated with `calloc` only at scheduler bootstrap, each sized exactly `ceil(actor_count / 64)` 64-bit words. Indexed admission, dispatch, and physical completion still synchronize under the same scheduler mutex as mailbox state and actor execution authority. There are no per-turn bitmap allocations and no tracing garbage collection.

## Admission and deterministic resource failure

- Reject zero actors and actor count above **65,536**, before allocating an actor or a bitmap.
- Reject zero or over-65,536 mailbox capacity, zero workers, or over-128 carriers.
- Preserve the existing checked `actor_count * mailbox_capacity` overflow rejection.
- For actor counts **above 1,024**, cap preallocated mailbox words at **1,048,576** (8 MiB); this avoids accepting a 65,536 x 65,536 allocation. Existing smaller VM admission limits remain unchanged.
- Bitmap allocation failure rejects the scheduler atomically, destroying any prior initialized runtime state and freeing both bitmap pointers. Successful quiescent destruction also frees both pointers. A bitmap bit is always checked against allocated word count before mutation.
- Actor generation, stop/drain/retire, CONTROL priority, DATA per-actor FIFO and exclusive carrier execution semantics remain unchanged.

## Native regression

`tests/native_actor_dynamic_ready_test.c` exercises actual native scheduling at 1,025; 4,097; 16,385; and **65,536** actor capacity. It sends DATA and CONTROL to actor IDs on both sides of each hierarchy/word boundary, including 1023/1024, 4095/4096, 16383/16384 and 65535. It checks `is_current_actor`, exact FIFO, message accounting, quiescent stop, retirement, destruction and over-budget/overflow admission rejection.

The existing 1,024-actor indexed regression is retained and updated to reject 65,537 (not 1,025) actors. All previous native actor CONTROL, creation-failure, wait, send/stop and sanitizer tests remain mandatory.

## Explicit limits

This increases the **maximum admitted logical actor-slot count**, not performance guarantees or number of concurrent pthreads. Global scheduler mutex and bitmap linear word scanning can still dominate CPU time. Sixteen thousand/fifty thousand idle lanes are inexpensive in the bitmap, but heap/authority initialization and the full ActorLane table remain proportional to capacity. The next performance increment should introduce hierarchical ready-summary indexes and per-NUMA sharding with verified actor-generation ownership.

The private exact-source test carrier is PR #24 in `ores-dummy-org-oreslang-stack-infra`; it contains 12 SHA-pinned native C files and existing exact Java topology tests. It is **not a full private source-tree mirror** and does not prove JVM/Graal JNI execution, LLVM AOT compiler execution or one-million-actor operation.
