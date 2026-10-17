#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native actor heap experimental ABI. Actor IDs and enter tokens MUST remain
 * private to the scheduler, not exposed as forgeable guest-language values.
 * One actor turn runs at a time and its memory authority is carrier-bound.
 * Migration requires leave + fresh enter; stable carrier threads are not
 * required. This heap does not use a tracing garbage collector. */
typedef struct OresActorI64Heap OresActorI64Heap;
typedef struct { uint64_t domain, epoch; } OresActorTurn;
typedef struct { uint64_t domain, index, generation; } OresActorSlot;
typedef struct { uint64_t domain, nonce, index, generation; } OresActorLoan;
typedef struct { uint64_t domain, index, generation; } OresActorExternalLease;

/* Allocated once per actor incarnation, survives any number of turns. */
OresActorI64Heap* ores_actor_i64_heap_open(uint64_t actor_id,
                                           uint64_t incarnation,
                                           uint64_t slot_capacity);
OresActorTurn ores_actor_i64_heap_enter(OresActorI64Heap* heap,
                                         uint64_t actor_id,
                                         uint64_t incarnation);
/* Scheduler-visible nonparking admission reasons. These are normal control
 * states, not actor panics; owner/incarnation forgery and epoch exhaustion
 * remain fatal contracts. */
typedef enum {
    ORES_ACTOR_TRY_LOCK_BUSY = 0,
    ORES_ACTOR_TRY_ENTERED = 1,
    ORES_ACTOR_TRY_CARRIER_OCCUPIED = 2,
    ORES_ACTOR_TRY_ACTOR_ACTIVE = 3,
    ORES_ACTOR_TRY_RETIRED = 4
} OresActorTryEnterResult;
OresActorTryEnterResult ores_actor_i64_heap_try_enter(
    OresActorI64Heap* heap, uint64_t actor_id, uint64_t incarnation,
    OresActorTurn* out);
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
/* Shared read loans survive turns/await. They forbid slot writes and drops,
 * plus physical heap release, until the last loan is released. A loan cannot
 * itself dereference memory outside an admitted turn. Exclusive mutable
 * loans are deliberately NOT exposed by this experimental scalar ABI. */
OresActorLoan ores_actor_i64_heap_borrow(OresActorI64Heap* heap,
                                          OresActorTurn turn, OresActorSlot slot);
void ores_actor_i64_heap_release(OresActorI64Heap* heap, OresActorLoan loan);
/* Lifetime-only authority for an admitted continuation/FFI/I/O callback.
 * It carries no slot index or memory dereference capability. Release may run
 * on any native carrier. The scheduler owns one mutable token per callback:
 * successful/stale release consumes it by zeroing the token, so retrying that
 * same token is safe even after CONTROL has destroyed the heap. Do not clone a
 * token into independently live owners; copied stale tokens are only supported
 * for replay/ABA rejection while the heap is still alive. */
OresActorExternalLease ores_actor_i64_heap_external_acquire(
    OresActorI64Heap* heap, OresActorTurn turn);
void ores_actor_i64_heap_external_release(
    OresActorI64Heap* heap, OresActorExternalLease* lease);
/* Compiler-emitted, serial actor field ABI. Only admitted turns may access
 * fixed-layout owned fields. Field initialization is single-use. */
void ores_actor_i64_heap_field_init(OresActorI64Heap* heap,
                                    uint64_t domain, uint64_t epoch,
                                    uint64_t index, int64_t initial);
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
/* Returns zero while a turn, read loan, or external lifetime lease is
 * outstanding; returns one and frees native memory after quiescence. Never
 * call with an invalidated heap pointer. The supervisor is responsible for
 * dropping its heap pointer after successful destroy and fencing stale users. */
int ores_actor_i64_heap_try_destroy(OresActorI64Heap* heap);
#ifdef __cplusplus
}
#endif
