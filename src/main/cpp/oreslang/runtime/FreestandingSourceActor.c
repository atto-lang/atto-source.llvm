#include "oreslang/runtime/FreestandingSourceActor.h"

/* Host-independent implementation of the compiler's existing actor-field ABI.
 * This single-core prototype is trusted-main-only: raw C pointers are not
 * guest capabilities. It has no heap, libc, POSIX, atomics or thread usage. */
struct OresActorI64Heap {
    uint64_t domain;
    uint64_t epoch;
    uint64_t *cells;
    uint32_t field_count;
    uint32_t init_mask;
    uint32_t mutable_mask;
    uint32_t in_turn;
    uint32_t initializing;
};
typedef struct {
    OresKernelRef ref;
    OresSourceActorMessage handler;
    struct OresActorI64Heap heap;
    uint32_t issued;
    uint32_t stopped;
} SourceSlot;
static OresKernel scheduler;
static SourceSlot slots[ORES_KERNEL_ACTORS];
static OresFreestandingActorStats summary;
static SourceSlot *current;
static uint32_t ready;
static uint32_t issued;

static void reject(void) {
    __builtin_trap();
    for (;;) {}
}
static void ensure_ready(void) {
    if (!ready) {
        if (ores_kernel_init(&scheduler) != ORES_KERNEL_OK) reject();
        ready = 1;
    }
}
static SourceSlot *get_actor(void *p) {
    for (uint32_t i = 0; i < issued; ++i) {
        if (p != (void *)&slots[i]) continue;
        SourceSlot *s = &slots[i];
        if (!s->issued || s->stopped || s->ref.owner != &scheduler ||
            s->ref.slot >= ORES_KERNEL_ACTORS) reject();
        OresKernelActor *k = &scheduler.actors[s->ref.slot];
        if (!k->active || k->generation != s->ref.generation) reject();
        return s;
    }
    reject();
    return 0;
}
static void begin(SourceSlot *s, uint32_t initializing) {
    if (current || s->heap.in_turn || s->heap.epoch == UINT64_MAX) reject();
    current = s;
    s->heap.in_turn = 1;
    s->heap.initializing = initializing;
    s->heap.epoch++;
}
static void finish(SourceSlot *s) {
    if (current != s || !s->heap.in_turn) reject();
    s->heap.in_turn = 0;
    s->heap.initializing = 0;
    current = 0;
}
static int deliver(uint64_t fields[ORES_KERNEL_FIELDS], int64_t payload,
                   int64_t *result) {
    for (uint32_t i = 0; i < issued; ++i) {
        SourceSlot *s = &slots[i];
        if (!s->issued || s->stopped || s->heap.cells != fields) continue;
        if (!s->handler || s->ref.owner != &scheduler ||
            scheduler.actors[s->ref.slot].generation != s->ref.generation)
            reject();
        begin(s, 0);
        *result = s->handler((OresActorI64Heap *)&s->heap,
                             s->heap.domain, s->heap.epoch, payload);
        finish(s);
        if (summary.delivered != UINT64_MAX) summary.delivered++;
        summary.last_result[s->ref.slot] = *result;
        return 1;
    }
    reject();
    return 0;
}
void *oreslang_source_actor_spawn(OresSourceActorInit init,
                                  OresSourceActorMessage handler,
                                  uint64_t fields) {
    ensure_ready();
    if (!init || !handler || fields > ORES_KERNEL_FIELDS ||
        issued >= ORES_KERNEL_ACTORS || scheduler.executing || current)
        reject();
    SourceSlot *s = &slots[issued];
    if (ores_kernel_spawn(&scheduler, deliver, &s->ref) != ORES_KERNEL_OK)
        reject();
    s->issued = 1;
    s->stopped = 0;
    s->handler = handler;
    s->heap.domain = ((uint64_t)s->ref.generation << 32) | (s->ref.slot + 1u);
    s->heap.epoch = 0;
    s->heap.cells = scheduler.actors[s->ref.slot].fields;
    s->heap.field_count = (uint32_t)fields;
    s->heap.init_mask = 0;
    s->heap.mutable_mask = 0;
    s->heap.in_turn = 0;
    issued++;
    begin(s, 1);
    init((OresActorI64Heap *)&s->heap, s->heap.domain, s->heap.epoch);
    finish(s);
    if (s->heap.init_mask != ((UINT32_C(1) << (uint32_t)fields) - 1u)) reject();
    summary.spawned++;
    return s;
}
void oreslang_source_actor_send(void *ref, int64_t payload) {
    if (current || scheduler.executing) reject();
    SourceSlot *s = get_actor(ref);
    if (ores_kernel_send(&scheduler, s->ref, payload) != ORES_KERNEL_OK)
        reject();
}
void oreslang_source_actor_stop(void *ref) {
    if (current || scheduler.executing) reject();
    SourceSlot *s = get_actor(ref);
    if (ores_kernel_stop(&scheduler, s->ref) != ORES_KERNEL_OK) reject();
    uint32_t left = ORES_KERNEL_ACTORS * (ORES_KERNEL_MAILBOX + 1u) + 1u;
    while (scheduler.actors[s->ref.slot].active &&
           scheduler.actors[s->ref.slot].generation == s->ref.generation) {
        if (!left--) reject();
        OresKernelTick t = ores_kernel_tick(&scheduler);
        if (t != ORES_KERNEL_DELIVERED && t != ORES_KERNEL_RETIRED) reject();
    }
    s->stopped = 1;
    s->heap.cells = 0;
    s->heap.domain = 0;
    s->heap.epoch = 0;
    s->heap.field_count = 0;
    s->heap.init_mask = 0;
    s->heap.mutable_mask = 0;
    s->heap.in_turn = 0;
    s->heap.initializing = 0;
    s->handler = 0;
    if (summary.retired != UINT32_MAX) summary.retired++;
}
static struct OresActorI64Heap *get_heap(OresActorI64Heap *heap,
        uint64_t domain, uint64_t epoch, uint64_t index) {
    if (!current || heap != (OresActorI64Heap *)&current->heap) reject();
    struct OresActorI64Heap *h = &current->heap;
    if (!current->issued || current->stopped || !h->in_turn || !h->cells ||
        h->domain != domain || h->epoch != epoch || index >= h->field_count ||
        current->ref.owner != &scheduler ||
        !scheduler.actors[current->ref.slot].active ||
        scheduler.actors[current->ref.slot].generation != current->ref.generation)
        reject();
    return h;
}
static void init_field(OresActorI64Heap *heap, uint64_t domain, uint64_t epoch,
                       uint64_t index, int64_t value, int mutable) {
    struct OresActorI64Heap *h = get_heap(heap, domain, epoch, index);
    uint32_t bit = UINT32_C(1) << (uint32_t)index;
    if (!h->initializing || (h->init_mask & bit)) reject();
    h->cells[index] = (uint64_t)value;
    h->init_mask |= bit;
    if (mutable) h->mutable_mask |= bit;
}
void ores_actor_i64_heap_field_init(OresActorI64Heap *heap, uint64_t domain,
        uint64_t epoch, uint64_t index, int64_t initial) {
    init_field(heap, domain, epoch, index, initial, 0);
}
void ores_actor_i64_heap_field_init_mutable(OresActorI64Heap *heap, uint64_t domain,
        uint64_t epoch, uint64_t index, int64_t initial) {
    init_field(heap, domain, epoch, index, initial, 1);
}
int64_t ores_actor_i64_heap_field_read(OresActorI64Heap *heap, uint64_t domain,
        uint64_t epoch, uint64_t index) {
    struct OresActorI64Heap *h = get_heap(heap, domain, epoch, index);
    if (!(h->init_mask & (UINT32_C(1) << (uint32_t)index))) reject();
    return (int64_t)h->cells[index];
}
void ores_actor_i64_heap_field_write(OresActorI64Heap *heap, uint64_t domain,
        uint64_t epoch, uint64_t index, int64_t value) {
    struct OresActorI64Heap *h = get_heap(heap, domain, epoch, index);
    const uint32_t bit = UINT32_C(1) << (uint32_t)index;
    if (!(h->init_mask & bit) || !(h->mutable_mask & bit) || h->initializing)
        reject();
    h->cells[index] = (uint64_t)value;
}
void ores_freestanding_source_stats(OresFreestandingActorStats *out) {
    if (!out || current || scheduler.executing) reject();
    out->spawned = summary.spawned;
    out->retired = summary.retired;
    out->delivered = summary.delivered;
    for (uint32_t i = 0; i < ORES_KERNEL_ACTORS; ++i)
        out->last_result[i] = summary.last_result[i];
}
