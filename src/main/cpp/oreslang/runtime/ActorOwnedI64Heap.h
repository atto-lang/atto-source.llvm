#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native actor heap experimental ABI. Actor IDs and enter tokens MUST remain
 * private to the scheduler, not exposed as forgeable guest-language values.
 * One actor turn runs at a time. This does not require stable carrier threads
 * and it does not use a tracing garbage collector. */
typedef struct OresActorI64Heap OresActorI64Heap;
typedef struct { uint64_t domain, epoch; } OresActorTurn;
typedef struct { uint64_t domain, index, generation; } OresActorSlot;
typedef struct { uint64_t domain, nonce, index, generation; } OresActorLoan;

/* HOST ONLY, experimental, scalar-i64 actor heap access evidence v1.
 * This records only operations through this heap ABI. It does NOT certify
 * complete Oreslang memory/synchronization coverage or race freedom.
 * Guest code must not receive this API, its output buffer, or credentials.
 */
typedef enum {
    ORES_ACTOR_HEAP_ACCESS_ALLOC = 1,
    ORES_ACTOR_HEAP_ACCESS_READ = 2,
    ORES_ACTOR_HEAP_ACCESS_WRITE = 3,
    ORES_ACTOR_HEAP_ACCESS_DROP = 4,
    ORES_ACTOR_HEAP_ACCESS_FIELD_INIT = 5,
    ORES_ACTOR_HEAP_ACCESS_FIELD_READ = 6,
    ORES_ACTOR_HEAP_ACCESS_FIELD_WRITE = 7
} OresActorHeapAccessKind;

typedef struct {
    uint64_t domain, actor_id, incarnation, turn_epoch;
    uint64_t slot_index, slot_generation;
    uint32_t kind;
} OresActorHeapAccessEvent;

typedef struct {
    uint64_t dropped;
    uint32_t stored, capacity;
    uint32_t enabled;
} OresActorHeapAccessSummary;

/* Enable before first turn; bounded 1..65536 events. Never allocates while
 * a turn executes. Returns 0 if unavailable/rejected; 1 only on admission.
 * Snapshot must be taken while no turn executes; refuses partial buffers.
 * Overflow is sticky (dropped>0); snapshots then return 0 (inconclusive)
 * while filling summary and any adequately sized diagnostic output buffer.
 * A return value of 1 confirms only buffer completeness, NOT program coverage.
 * Neither function exposes a native/guest address or recorded data value.
 */
int ores_actor_i64_heap_trace_enable(OresActorI64Heap* heap, uint32_t capacity);
int ores_actor_i64_heap_trace_snapshot(
    OresActorI64Heap* heap, OresActorHeapAccessEvent* output,
    uint32_t output_capacity, OresActorHeapAccessSummary* summary);

/* Allocated once per actor incarnation, survives any number of turns. */
OresActorI64Heap* ores_actor_i64_heap_open(uint64_t actor_id,
                                           uint64_t incarnation,
                                           uint64_t slot_capacity);
OresActorTurn ores_actor_i64_heap_enter(OresActorI64Heap* heap,
                                         uint64_t actor_id,
                                         uint64_t incarnation);
void ores_actor_i64_heap_leave(OresActorI64Heap* heap, OresActorTurn turn);
OresActorSlot ores_actor_i64_heap_new(OresActorI64Heap* heap,
                                       OresActorTurn turn, int64_t value);
int64_t ores_actor_i64_heap_read(OresActorI64Heap* heap,
                                  OresActorTurn turn, OresActorSlot slot);
void ores_actor_i64_heap_write(OresActorI64Heap* heap,
                                 OresActorTurn turn,
                                 OresActorSlot slot, int64_t value);
void ores_actor_i64_heap_drop(OresActorI64Heap* heap,
                                OresActorTurn turn, OresActorSlot slot);
/* Compiler-owned actor-handler temporary ABI: the compiler allocates a
 * three-word token on the native stack and drops it before handler exit.
 * Tokens may not escape across await, FFI, or an actor turn boundary. */
void ores_actor_i64_heap_scoped_new(OresActorI64Heap* heap,
    uint64_t domain, uint64_t epoch, int64_t value, uint64_t token[3]);
void ores_actor_i64_heap_scoped_drop(OresActorI64Heap* heap,
    uint64_t domain, uint64_t epoch, uint64_t token[3]);
/* Shared read loans survive turns/await. They forbid slot writes and drops,
 * plus physical heap release, until the last loan is released. A loan cannot
 * itself dereference memory outside an admitted turn. Exclusive mutable
 * loans are exposed only through a separate turn-local, compiler-owned ABI. */
OresActorLoan ores_actor_i64_heap_borrow(OresActorI64Heap* heap,
                                          OresActorTurn turn, OresActorSlot slot);
void ores_actor_i64_heap_release(OresActorI64Heap* heap, OresActorLoan loan);
/* Experimental exclusive mutable loan, for compiler/scheduler use ONLY.
 * No raw pointer crosses this boundary, and these are not cross-actor shared
 * memory leases. Exactly one writer, zero readers; multiple shared readers
 * are allowed only with zero writers. An immutable actor field cannot be
 * mutably borrowed. A mutable loan is bound to the current actor turn:
 * release it before actor_leave / suspension; no mutable loan crosses await.
 * Source-level borrow checking, VM generation fencing and cancellation
 * remain mandatory before exposing this ABI to general guest programs.
 * Violations fail closed (process abort in this experimental host runtime).
 */
/* Attempt exclusive access without a carrier-blocking wait or process abort
 * on ordinary shared-reader/exclusive-writer contention (or loan-capacity
 * saturation). Returns 1 and a valid token on success; returns 0 and a zero
 * token on contention. Invalid actor identity, stale/forged slot credentials,
 * immutable fields and nonce exhaustion remain fatal *host* invariants.
 * This is not yet source-level checked borrowing, actor-local trapping, or a
 * cross-actor shared-memory protocol. */
int ores_actor_i64_heap_try_borrow_mut(OresActorI64Heap* heap,
                                       OresActorTurn turn, OresActorSlot slot,
                                       OresActorLoan* out);
OresActorLoan ores_actor_i64_heap_borrow_mut(OresActorI64Heap* heap,
                                              OresActorTurn turn, OresActorSlot slot);
/* Typed host ABI for compiler-lowered actor fault reporting.
 * Unlike the older fatal loan_* calls, BAD_TURN/BAD_HANDLE/RETIRED return
 * without mutating slot state or aborting the whole native process.
 * BUSY is one failed heap lock attempt; the actor scheduler decides whether
 * to retry after yielding. A failed read clears *out_value.
 * Only a live, supervisor-pinned heap pointer is valid (this does not
 * recover a dangling heap pointer or replace a source borrow checker).
 * Return values must be checked by generated code and routed to actor-local
 * terminal failure handling; ignoring a status is NOT safe. */
typedef enum {
    ORES_ACTOR_LOAN_OK = 0,
    ORES_ACTOR_LOAN_BUSY = 1,
    ORES_ACTOR_LOAN_BAD_TURN = 2,
    ORES_ACTOR_LOAN_BAD_HANDLE = 3,
    ORES_ACTOR_LOAN_RETIRED = 4
} OresActorLoanStatus;
OresActorLoanStatus ores_actor_i64_heap_try_loan_read_mut(
    OresActorI64Heap* heap, OresActorTurn turn, OresActorLoan loan,
    int64_t* out_value);
OresActorLoanStatus ores_actor_i64_heap_try_loan_write(
    OresActorI64Heap* heap, OresActorTurn turn, OresActorLoan loan,
    int64_t value);
/* Supervisor-only terminal transition after a failed guest turn.
 * Requires its authenticated, current turn. Returns BUSY on contended heap
 * mutex, BAD_TURN for stale generation/epoch, RETIRED if already fenced.
 * On OK, revokes this turn's exclusive loans, retires the heap and ends the
 * turn; no subsequent turn may enter. Shared reader loans remain live and
 * must be separately released before physical heap destruction.
 * No guest may invoke this directly. This is NOT an actor-local trap on its
 * own: generated guest code and supervisor delivery must use these statuses
 * and fence all asynchronous callbacks before attempting reclamation. */
OresActorLoanStatus ores_actor_i64_heap_fault_turn(
    OresActorI64Heap* heap, OresActorTurn turn);
int64_t ores_actor_i64_heap_loan_read_mut(OresActorI64Heap* heap,
                                           OresActorTurn turn, OresActorLoan loan);
void ores_actor_i64_heap_loan_write(OresActorI64Heap* heap,
                                    OresActorTurn turn, OresActorLoan loan,
                                    int64_t value);
/* Compiler-emitted, serial actor field ABI. Only admitted turns may access
 * fixed-layout owned fields. Field initialization is single-use. */
void ores_actor_i64_heap_field_init(OresActorI64Heap* heap,
                                    uint64_t domain, uint64_t epoch,
                                    uint64_t index, int64_t initial);
/* Failure-aware compiler field ABI: field reads/writes report the same
 * typed status as exclusive loans. Does not abort on stale turn, retired
 * heap, field bounds/ownership denial or heap-lock contention. Failed reads
 * zero *out_value; failed writes never modify field contents. */
OresActorLoanStatus ores_actor_i64_heap_try_field_read(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
    uint64_t index, int64_t* out_value);
OresActorLoanStatus ores_actor_i64_heap_try_field_write(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
    uint64_t index, int64_t value);
int64_t ores_actor_i64_heap_field_read(OresActorI64Heap* heap,
                                       uint64_t domain, uint64_t epoch,
                                       uint64_t index);
/* Only compiler-declared mutable fields are writable.
 * Outstanding loans prohibit writes, including after a suspended turn. */
void ores_actor_i64_heap_field_init_mutable(OresActorI64Heap* heap,
                                            uint64_t domain, uint64_t epoch,
                                            uint64_t index, int64_t initial);
void ores_actor_i64_heap_field_write(OresActorI64Heap* heap,
                                     uint64_t domain, uint64_t epoch,
                                     uint64_t index, int64_t value);
void ores_actor_i64_heap_retire(OresActorI64Heap* heap);
/* Returns zero while a turn or loan is outstanding; returns one and frees
 * native memory after quiescence. Never call with an invalidated heap pointer.
 * The supervisor is responsible for fencing stale wakeups/callback holders. */
int ores_actor_i64_heap_try_destroy(OresActorI64Heap* heap);
#ifdef __cplusplus
}
#endif
