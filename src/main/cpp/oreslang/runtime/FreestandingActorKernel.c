#include "oreslang/runtime/FreestandingActorKernel.h"

/* Detect damaged queue metadata before a bad head can index outside the
 * statically allocated mailbox.  The kernel trusts its owner-provided RAM,
 * but fails closed on detectable damage instead of attempting delivery. */
static int metadata_valid(const OresKernelActor *actor) {
    return actor->head < ORES_KERNEL_MAILBOX &&
           actor->length <= ORES_KERNEL_MAILBOX &&
           actor->closing <= 1u && actor->handler != 0;
}

static void record_fault(OresKernel *kernel) {
    if (kernel->fault_count != UINT32_MAX) kernel->fault_count++;
}

static OresKernelActor *locate(OresKernel *kernel, OresKernelRef ref) {
    if (kernel == 0 || kernel->magic != ORES_KERNEL_MAGIC ||
        ref.slot >= ORES_KERNEL_ACTORS || ref.generation == 0u) return 0;
    OresKernelActor *actor = &kernel->actors[ref.slot];
    if (actor->active != 1u || actor->generation != ref.generation ||
        !metadata_valid(actor)) return 0;
    return actor;
}

static void retire(OresKernelActor *actor) {
    /* Clear owned data and queued messages BEFORE invalidating the ref. */
    /* Volatile writes retain zeroization even under optimizing/LTO builds. */
    volatile uint64_t *const fields = actor->fields;
    volatile int64_t *const messages = actor->messages;
    for (uint32_t i=0; i<ORES_KERNEL_FIELDS; ++i) fields[i] = 0;
    for (uint32_t i=0; i<ORES_KERNEL_MAILBOX; ++i) messages[i] = 0;
    actor->handler = 0;
    actor->head = 0;
    actor->length = 0;
    actor->closing = 0;
    actor->processed = 0;
    actor->last_result = 0;
    actor->active = 0;
}

OresKernelStatus ores_kernel_init(OresKernel *kernel) {
    if (kernel == 0) return ORES_KERNEL_INVALID;
    const int reinitializing = kernel->magic == ORES_KERNEL_MAGIC;
    if (reinitializing && kernel->executing) return ORES_KERNEL_BUSY;
    kernel->magic = 0; /* Fail closed while clearing. */
    kernel->next_slot = 0;
    kernel->executing = 0;
    kernel->fault_count = 0;
    for (uint32_t n=0; n<ORES_KERNEL_ACTORS; ++n) {
        OresKernelActor *actor = &kernel->actors[n];
        /* A previous handle must not revive after repeated initialization. */
        uint32_t generation = reinitializing ? actor->generation : 0;
        if (reinitializing && generation != UINT32_MAX) generation++;
        retire(actor);
        actor->generation = generation;
    }
    kernel->magic = ORES_KERNEL_MAGIC;
    return ORES_KERNEL_OK;
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
        if (actor->active == 0u) continue;
        if (actor->active != 1u || !metadata_valid(actor)) {
            retire(actor);
            record_fault(kernel);
            kernel->next_slot = (slot + 1) % ORES_KERNEL_ACTORS;
            kernel->executing = 0;
            return ORES_KERNEL_HANDLER_FAILED;
        }
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
            record_fault(kernel);
            kernel->executing = 0;
            return ORES_KERNEL_HANDLER_FAILED;
        }
        actor->last_result = result;
        if (actor->processed != UINT64_MAX) actor->processed++;
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
