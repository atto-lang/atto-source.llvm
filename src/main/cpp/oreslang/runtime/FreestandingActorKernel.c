#include "oreslang/runtime/FreestandingActorKernel.h"

static OresKernelActor *locate(OresKernel *kernel, OresKernelRef ref) {
    if (kernel == 0 || kernel->magic != ORES_KERNEL_MAGIC ||
        ref.slot >= ORES_KERNEL_ACTORS || ref.generation == 0u) return 0;
    OresKernelActor *actor = &kernel->actors[ref.slot];
    if (actor->active != 1u || actor->generation != ref.generation) return 0;
    return actor;
}

static void retire(OresKernelActor *actor) {
    /* Clear owned data and queued messages BEFORE invalidating the ref. */
    for (uint32_t i=0; i<ORES_KERNEL_FIELDS; ++i) actor->fields[i] = 0;
    for (uint32_t i=0; i<ORES_KERNEL_MAILBOX; ++i) actor->messages[i] = 0;
    actor->handler = 0;
    actor->head = 0;
    actor->length = 0;
    actor->closing = 0;
    actor->processed = 0;
    actor->last_result = 0;
    actor->active = 0;
}

void ores_kernel_init(OresKernel *kernel) {
    if (kernel == 0) return;
    kernel->magic = 0; /* Fail closed while clearing. */
    kernel->next_slot = 0;
    kernel->executing = 0;
    kernel->fault_count = 0;
    for (uint32_t n=0; n<ORES_KERNEL_ACTORS; ++n) {
        OresKernelActor *actor = &kernel->actors[n];
        actor->generation = 0;
        retire(actor);
    }
    kernel->magic = ORES_KERNEL_MAGIC;
}

OresKernelStatus ores_kernel_spawn(OresKernel *kernel, OresKernelHandler handler,
                                   OresKernelRef *out) {
    if (kernel == 0 || kernel->magic != ORES_KERNEL_MAGIC ||
        out == 0 || handler == 0) return ORES_KERNEL_INVALID;
    if (kernel->executing) return ORES_KERNEL_BUSY;
    for (uint32_t k=0; k<ORES_KERNEL_ACTORS; ++k) {
        uint32_t slot = (kernel->next_slot + k) % ORES_KERNEL_ACTORS;
        OresKernelActor *actor = &kernel->actors[slot];
        if (actor->active || actor->generation == UINT32_MAX) continue;
        /* Never wrap a generation; a stale ref must never be revived. */
        actor->generation++;
        actor->active = 1;
        actor->handler = handler;
        actor->closing = 0;
        actor->head = 0;
        actor->length = 0;
        actor->processed = 0;
        actor->last_result = 0;
        for (uint32_t i=0; i<ORES_KERNEL_FIELDS; ++i) actor->fields[i] = 0;
        for (uint32_t i=0; i<ORES_KERNEL_MAILBOX; ++i) actor->messages[i] = 0;
        out->slot = slot;
        out->generation = actor->generation;
        kernel->next_slot = (slot + 1) % ORES_KERNEL_ACTORS;
        return ORES_KERNEL_OK;
    }
    return ORES_KERNEL_FULL;
}

OresKernelStatus ores_kernel_send(OresKernel *kernel, OresKernelRef ref,
                                  int64_t payload) {
    if (kernel && kernel->magic == ORES_KERNEL_MAGIC && kernel->executing)
        return ORES_KERNEL_BUSY;
    OresKernelActor *actor = locate(kernel, ref);
    if (!actor) return ORES_KERNEL_INVALID;
    if (actor->closing) return ORES_KERNEL_CLOSED;
    if (actor->length >= ORES_KERNEL_MAILBOX) return ORES_KERNEL_FULL;
    const uint32_t tail = (actor->head + actor->length) % ORES_KERNEL_MAILBOX;
    actor->messages[tail] = payload;
    actor->length++;
    return ORES_KERNEL_OK;
}

OresKernelStatus ores_kernel_stop(OresKernel *kernel, OresKernelRef ref) {
    if (kernel && kernel->magic == ORES_KERNEL_MAGIC && kernel->executing)
        return ORES_KERNEL_BUSY;
    OresKernelActor *actor = locate(kernel, ref);
    if (!actor) return ORES_KERNEL_INVALID;
    if (actor->closing) return ORES_KERNEL_CLOSED;
    actor->closing = 1; /* Drains already admitted messages. */
    return ORES_KERNEL_OK;
}

OresKernelTick ores_kernel_tick(OresKernel *kernel) {
    if (kernel == 0 || kernel->magic != ORES_KERNEL_MAGIC) return ORES_KERNEL_REENTRANT;
    if (kernel->executing) return ORES_KERNEL_REENTRANT;
    kernel->executing = 1;
    for (uint32_t k=0; k<ORES_KERNEL_ACTORS; ++k) {
        const uint32_t slot = (kernel->next_slot + k) % ORES_KERNEL_ACTORS;
        OresKernelActor *actor = &kernel->actors[slot];
        if (!actor->active) continue;
        if (actor->length == 0) {
            if (actor->closing) {
                retire(actor);
                kernel->next_slot = (slot + 1) % ORES_KERNEL_ACTORS;
                kernel->executing = 0;
                return ORES_KERNEL_RETIRED;
            }
            continue;
        }
        const int64_t payload = actor->messages[actor->head];
        actor->messages[actor->head] = 0;
        actor->head = (actor->head + 1) % ORES_KERNEL_MAILBOX;
        actor->length--;
        int64_t result = 0;
        const int ok = actor->handler(actor->fields, payload, &result);
        kernel->next_slot = (slot + 1) % ORES_KERNEL_ACTORS;
        if (!ok) {
            retire(actor);
            kernel->fault_count++;
            kernel->executing = 0;
            return ORES_KERNEL_HANDLER_FAILED;
        }
        actor->last_result = result;
        actor->processed++;
        kernel->executing = 0;
        return ORES_KERNEL_DELIVERED;
    }
    kernel->executing = 0;
    return ORES_KERNEL_IDLE;
}

OresKernelStatus ores_kernel_snapshot(const OresKernel *kernel, OresKernelRef ref,
                                      uint64_t *processed, int64_t *last_result) {
    if (!processed || !last_result) return ORES_KERNEL_INVALID;
    if (kernel && kernel->magic == ORES_KERNEL_MAGIC && kernel->executing)
        return ORES_KERNEL_BUSY;
    OresKernelActor *actor = locate((OresKernel *)kernel, ref);
    if (!actor) return ORES_KERNEL_INVALID;
    *processed = actor->processed;
    *last_result = actor->last_result;
    return ORES_KERNEL_OK;
}
