#include "oreslang/runtime/FreestandingActorKernel.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t generation;
    uint32_t active;
    uint32_t closing;
    uint32_t count;
    int64_t pending[ORES_KERNEL_MAILBOX];
    uint64_t sum;
    uint64_t processed;
    int64_t last;
} ModelActor;
typedef struct {
    uint32_t next;
    const OresKernel *owner;
    ModelActor actors[ORES_KERNEL_ACTORS];
} Model;

static uint32_t seed = UINT32_C(0xc0ffee13);
static uint32_t random32(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static int handler(uint64_t fields[ORES_KERNEL_FIELDS], int64_t message, int64_t *result) {
    fields[0] += (uint64_t)message;
    *result = (int64_t)fields[0];
    return 1;
}
static OresKernelStatus model_spawn(Model *m, OresKernelRef *ref) {
    for (uint32_t k = 0; k < ORES_KERNEL_ACTORS; ++k) {
        uint32_t s = (m->next + k) % ORES_KERNEL_ACTORS;
        ModelActor *a = &m->actors[s];
        if (a->active || a->generation == UINT32_MAX) continue;
        a->generation++;
        a->active = 1;
        a->closing = 0;
        a->count = 0;
        a->sum = 0;
        a->processed = 0;
        a->last = 0;
        ref->slot = s;
        ref->generation = a->generation;
        ref->owner = m->owner;
        m->next = (s + 1) % ORES_KERNEL_ACTORS;
        return ORES_KERNEL_OK;
    }
    return ORES_KERNEL_FULL;
}
static ModelActor *lookup(Model *m, OresKernelRef ref) {
    if (ref.slot >= ORES_KERNEL_ACTORS || ref.generation == 0 ||
        ref.owner != m->owner) return 0;
    ModelActor *a = &m->actors[ref.slot];
    return a->active && a->generation == ref.generation ? a : 0;
}
static OresKernelStatus model_send(Model *m, OresKernelRef ref, int64_t payload) {
    ModelActor *a = lookup(m,ref);
    if (!a) return ORES_KERNEL_INVALID;
    if (a->closing) return ORES_KERNEL_CLOSED;
    if (a->count >= ORES_KERNEL_MAILBOX) return ORES_KERNEL_FULL;
    a->pending[a->count++] = payload;
    return ORES_KERNEL_OK;
}
static OresKernelStatus model_stop(Model *m, OresKernelRef ref) {
    ModelActor *a = lookup(m,ref);
    if (!a) return ORES_KERNEL_INVALID;
    if (a->closing) return ORES_KERNEL_CLOSED;
    a->closing = 1;
    return ORES_KERNEL_OK;
}
static OresKernelTick model_tick(Model *m) {
    for (uint32_t k = 0; k < ORES_KERNEL_ACTORS; ++k) {
        const uint32_t slot = (m->next + k) % ORES_KERNEL_ACTORS;
        ModelActor *a = &m->actors[slot];
        if (!a->active) continue;
        if (!a->count) {
            if (!a->closing) continue;
            const uint32_t gen = a->generation;
            *a = (ModelActor){0};
            a->generation = gen;
            m->next = (slot + 1) % ORES_KERNEL_ACTORS;
            return ORES_KERNEL_RETIRED;
        }
        const int64_t payload = a->pending[0];
        for (uint32_t i=1; i<a->count; ++i) a->pending[i-1] = a->pending[i];
        a->count--;
        a->sum += (uint64_t)payload;
        a->last = (int64_t)a->sum;
        if (a->processed != UINT64_MAX) a->processed++;
        m->next = (slot + 1) % ORES_KERNEL_ACTORS;
        return ORES_KERNEL_DELIVERED;
    }
    return ORES_KERNEL_IDLE;
}
static void model_reset(Model *m) {
    m->next = 0;
    for (uint32_t slot=0; slot<ORES_KERNEL_ACTORS; ++slot) {
        uint32_t gen = m->actors[slot].generation;
        if (gen != UINT32_MAX) gen++;
        m->actors[slot] = (ModelActor){0};
        m->actors[slot].generation = gen;
    }
}
static void invariants(const OresKernel *vm, const Model *m) {
    assert(vm->magic == ORES_KERNEL_MAGIC);
    assert(vm->executing == 0);
    assert(vm->next_slot == m->next);
    assert(vm->fault_count == 0);
    for (uint32_t slot=0; slot<ORES_KERNEL_ACTORS; ++slot) {
        const OresKernelActor *a = &vm->actors[slot];
        const ModelActor *b = &m->actors[slot];
        assert(a->active == b->active);
        assert(a->generation == b->generation);
        assert(a->closing == b->closing);
        assert(a->length == b->count);
        assert(a->processed == b->processed);
        assert(a->last_result == b->last);
        assert(a->fields[0] == b->sum);
        for (uint32_t f=1; f<ORES_KERNEL_FIELDS; ++f) assert(a->fields[f] == 0);
        if (!a->active) {
            assert(a->handler == 0);
            for (uint32_t q=0; q<ORES_KERNEL_MAILBOX; ++q) assert(a->messages[q] == 0);
        } else {
            assert(a->handler == handler);
            assert(a->head < ORES_KERNEL_MAILBOX);
            for (uint32_t q=0; q<b->count; ++q)
                assert(a->messages[(a->head + q) % ORES_KERNEL_MAILBOX] == b->pending[q]);
        }
    }
}
int main(void) {
    OresKernel vm = {0};
    Model m = {0};
    m.owner = &vm;
    OresKernelRef handles[512] = {{0}};
    uint32_t total = 0, writes = 0;
    assert(ores_kernel_init(&vm) == ORES_KERNEL_OK);
    for (uint32_t i=0; i<100000; ++i) {
        const uint32_t random = random32();
        const uint32_t op = random % 100;
        OresKernelRef h = total ? handles[random32() % total] : (OresKernelRef){0};
        if (op < 15) {
            OresKernelRef actual={0}, expected={0};
            OresKernelStatus a=ores_kernel_spawn(&vm,handler,&actual);
            OresKernelStatus b=model_spawn(&m,&expected);
            assert(a==b);
            if (a==ORES_KERNEL_OK) {
                assert(actual.slot==expected.slot && actual.generation==expected.generation &&
                       actual.owner==expected.owner);
                handles[writes++ % 512] = actual;
                if (total<512) total++;
            }
        } else if (op<55) {
            const int64_t payload = (int64_t)(random32()%6);
            assert(ores_kernel_send(&vm,h,payload)==model_send(&m,h,payload));
        } else if (op<73) {
            assert(ores_kernel_stop(&vm,h)==model_stop(&m,h));
        } else if (op<94) {
            assert(ores_kernel_tick(&vm)==model_tick(&m));
        } else if (op<98) {
            uint64_t processed=UINT64_MAX;
            int64_t last=INT64_MIN;
            const ModelActor *a=lookup(&m,h);
            const OresKernelStatus status=ores_kernel_snapshot(&vm,h,&processed,&last);
            assert(status==(a?ORES_KERNEL_OK:ORES_KERNEL_INVALID));
            if(a) assert(processed==a->processed && last==a->last);
        } else {
            assert(ores_kernel_init(&vm)==ORES_KERNEL_OK);
            model_reset(&m);
        }
        invariants(&vm,&m);
    }
    puts("FREESTANDING_ACTOR_KERNEL_MODEL_PASS 100000 actions");
    return 0;
}
