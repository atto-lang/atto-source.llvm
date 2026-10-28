#include "oreslang/runtime/ActorTensorI64.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void reject(void) { abort(); }

int ores_actor_tensor_i64_valid_span(int64_t n, int64_t start,
                                    int64_t count, int64_t stride) {
    if (n < 0 || start < 0 || count < 0 || stride <= 0 || start > n)
        return 0;
    if (count == 0) return 1;
    if (start == n) return 0;
    return count - 1 <= (n - 1 - start) / stride;
}

static void require_owner(const OresActorTensorI64* tensor,
                          OresActorI64Heap* heap) {
    if (!tensor || !tensor->live || !heap || tensor->owner != heap
        || tensor->length > ORES_ACTOR_TENSOR_I64_MAX_ELEMENTS)
        reject();
}

static uint32_t checked_lane(const OresActorTensorI64* tensor,
                             int64_t start, int64_t count, int64_t stride,
                             int64_t lane) {
    if (!ores_actor_tensor_i64_valid_span(tensor->length, start, count, stride)
        || lane < 0 || lane >= count)
        reject();
    /* Validated last element bounds imply this multiplication and sum fit
     * signed i64; every lane is between 0 and tensor->length - 1. */
    return (uint32_t)(start + lane * stride);
}

void ores_actor_tensor_i64_init(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    const int64_t* initial_values, uint32_t length) {
    if (!tensor || !heap || tensor->owner || tensor->live
        || length > ORES_ACTOR_TENSOR_I64_MAX_ELEMENTS
        || (length != 0 && !initial_values))
        reject();
    /* The actor heap authorizes this turn on every allocation. It remains
     * the actual physical backing; our descriptor holds only slot tokens. */
    for (uint32_t i = 0; i < length; ++i)
        tensor->slots[i] = ores_actor_i64_heap_new(heap, turn, initial_values[i]);
    tensor->owner = heap;
    tensor->length = length;
    atomic_store_explicit(&tensor->active_loans, 0, memory_order_release);
    tensor->live = 1;
}

int64_t ores_actor_tensor_i64_read(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    int64_t start, int64_t count, int64_t stride, int64_t lane) {
    require_owner(tensor, heap);
    return ores_actor_i64_heap_read(heap, turn,
        tensor->slots[checked_lane(tensor, start, count, stride, lane)]);
}

void ores_actor_tensor_i64_write(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    int64_t start, int64_t count, int64_t stride,
    int64_t lane, int64_t value) {
    require_owner(tensor, heap);
    const uint32_t index = checked_lane(tensor, start, count, stride, lane);
    /* Conservative whole-tensor shared loan: cannot mutate any lane while
     * suspended readers own a lease. Underlying heap enforces each slot too. */
    if (atomic_load_explicit(&tensor->active_loans, memory_order_acquire) != 0)
        reject();
    ores_actor_i64_heap_write(heap, turn, tensor->slots[index], value);
}

void ores_actor_tensor_i64_borrow_read(OresActorTensorI64* tensor,
    OresActorI64Heap* heap, OresActorTurn turn,
    OresActorTensorI64Loan* loan) {
    require_owner(tensor, heap);
    if (!loan || loan->active || tensor->length == 0) reject();
    /* This counter fences whole-owner take/drop. There is no loan to expose
     * to Oreslang source yet; the compiler must control descriptor lifetime. */
    if (atomic_fetch_add_explicit(&tensor->active_loans, 1,
            memory_order_acq_rel) == UINT32_MAX) reject();
    for (uint32_t i = 0; i < tensor->length; ++i)
        loan->slots[i] = ores_actor_i64_heap_borrow(heap, turn, tensor->slots[i]);
    loan->length = tensor->length;
    loan->tensor = tensor;
    loan->active = 1;
}

void ores_actor_tensor_i64_release(OresActorTensorI64Loan* loan) {
    if (!loan || !loan->active || !loan->tensor) reject();
    OresActorTensorI64* tensor = loan->tensor;
    if (!tensor->live || loan->length != tensor->length) reject();
    for (uint32_t i = 0; i < loan->length; ++i)
        ores_actor_i64_heap_release(tensor->owner, loan->slots[i]);
    if (atomic_fetch_sub_explicit(&tensor->active_loans, 1,
            memory_order_acq_rel) == 0) reject();
    memset(loan, 0, sizeof(*loan));
}

void ores_actor_tensor_i64_take(OresActorTensorI64* target,
                                OresActorTensorI64* source) {
    if (!target || !source || target == source || target->owner || target->live
        || !source->live || !source->owner
        || atomic_load_explicit(&source->active_loans, memory_order_acquire))
        reject();
    target->owner = source->owner;
    target->length = source->length;
    memcpy(target->slots, source->slots, sizeof(target->slots));
    atomic_store_explicit(&target->active_loans, 0, memory_order_release);
    target->live = 1;
    source->owner = NULL;
    source->length = 0;
    memset(source->slots, 0, sizeof(source->slots));
    source->live = 0;
}

void ores_actor_tensor_i64_drop(OresActorTensorI64* tensor,
                                OresActorI64Heap* heap, OresActorTurn turn) {
    require_owner(tensor, heap);
    if (atomic_load_explicit(&tensor->active_loans, memory_order_acquire))
        reject();
    for (uint32_t i = 0; i < tensor->length; ++i)
        ores_actor_i64_heap_drop(heap, turn, tensor->slots[i]);
    tensor->owner = NULL;
    tensor->length = 0;
    memset(tensor->slots, 0, sizeof(tensor->slots));
    tensor->live = 0;
}
