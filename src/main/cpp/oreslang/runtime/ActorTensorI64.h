#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif

/* HOST/COMPILER INTERNAL proof-of-concept, NOT a guest-visible C ABI.
 * Tensor elements are real actor-heap slots (bounded 32 x i64), not a
 * contiguous SIMD/GPU allocation. Runtime must supply an authenticated
 * actor turn, and the compiler must keep descriptors/loans unforgeable.
 * No compiler lowering to this adapter is claimed by this change.
 * Loan descriptors and their referenced tensor descriptor MUST reside in a
 * pinned lifetime-stable runtime frame until every loan is released; an
 * ordinary stack local may not outlive its owner when an actor suspends.
 * Actor teardown must fence all callbacks before physical heap destruction.
 * This ABI is not safe against arbitrary forged host pointers or memcpy
 * of descriptors; source guests must never receive these raw C structs.
 */
#define ORES_ACTOR_TENSOR_I64_MAX_ELEMENTS 32u
typedef struct {
    OresActorI64Heap* owner;
    OresActorSlot slots[ORES_ACTOR_TENSOR_I64_MAX_ELEMENTS];
    uint32_t length;
    _Atomic uint32_t active_loans;
    uint32_t live;
} OresActorTensorI64;

typedef struct {
    OresActorTensorI64* tensor;
    OresActorLoan slots[ORES_ACTOR_TENSOR_I64_MAX_ELEMENTS];
    uint32_t length;
    uint32_t active;
} OresActorTensorI64Loan;

/* Exact signed-64 bounds preflight: no pointer or ownership authority. */
int ores_actor_tensor_i64_valid_span(
    int64_t capacity, int64_t start, int64_t count, int64_t stride);

/* Caller initializes each descriptor with {0}. Operations require an active
 * actor turn for the underlying heap; invalid/stale/foreign accesses abort.
 * The source borrow checker must still prohibit copying descriptors. */
void ores_actor_tensor_i64_init(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    const int64_t* initial_values, uint32_t length);
int64_t ores_actor_tensor_i64_read(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    int64_t start, int64_t count, int64_t stride, int64_t lane);
void ores_actor_tensor_i64_write(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    int64_t start, int64_t count, int64_t stride,
    int64_t lane, int64_t value);

/* Conservative shared loan pins the ENTIRE tensor, including across turns.
 * All writes, transfer, and physical drop must wait for its release.
 * A zero-length tensor does not retain a physical allocation and cannot be
 * used as a heap-lifetime pin. */
void ores_actor_tensor_i64_borrow_read(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    OresActorTensorI64Loan* loan);
void ores_actor_tensor_i64_release(OresActorTensorI64Loan* loan);
void ores_actor_tensor_i64_take(OresActorTensorI64* target,
    OresActorTensorI64* source);
void ores_actor_tensor_i64_drop(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn);
#ifdef __cplusplus
}
#endif
