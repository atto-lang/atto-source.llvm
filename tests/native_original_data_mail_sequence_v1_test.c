#include "oreslang/runtime/NativeActorScheduler.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actors[2];
    _Atomic uint64_t seen[2];
    _Atomic int valid;
} Scenario;

static void receive(void* userdata, size_t actor_index, uint64_t payload) {
    Scenario* s = (Scenario*)userdata;
    assert(actor_index < 2);
    OresNativeActorRef actor = s->actors[actor_index];
    uint64_t original = 0;
    const int admitted = ores_native_actor_scheduler_current_data_mail_sequence(
        s->scheduler, actor, &original);
    uint64_t expected = atomic_fetch_add_explicit(
        &s->seen[actor_index], UINT64_C(1), memory_order_relaxed) + UINT64_C(1);
    if (!admitted || original != expected || original == 0)
        atomic_store_explicit(&s->valid, 0, memory_order_relaxed);

    // Identical user payloads must carry DIFFERENT immutable admission IDs.
    // A guest-controlled payload must never determine original_mail_sequence.
    if (actor_index == 0 && payload != UINT64_C(333))
        atomic_store_explicit(&s->valid, 0, memory_order_relaxed);

    uint64_t untouched = UINT64_C(0xdefaced);
    const OresNativeActorRef wrong = s->actors[1 - actor_index];
    if (ores_native_actor_scheduler_current_data_mail_sequence(
            s->scheduler, wrong, &untouched)
            || untouched != UINT64_C(0xdefaced))
        atomic_store_explicit(&s->valid, 0, memory_order_relaxed);
}

int main(void) {
    Scenario s = {0};
    atomic_init(&s.seen[0], 0);
    atomic_init(&s.seen[1], 0);
    atomic_init(&s.valid, 1);
    s.scheduler = ores_native_actor_scheduler_open(2, 8, 2, receive, &s);
    assert(s.scheduler);
    s.actors[0] = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    s.actors[1] = ores_native_actor_scheduler_spawn(s.scheduler, 1);
    assert(s.actors[0].scheduler_domain && s.actors[1].scheduler_domain);
    for (int n = 0; n < 5; ++n)
        assert(ores_native_actor_scheduler_try_send(
            s.scheduler, s.actors[0], UINT64_C(333)) == ORES_NATIVE_SEND_ACCEPTED);
    for (int n = 0; n < 3; ++n)
        assert(ores_native_actor_scheduler_try_send(
            s.scheduler, s.actors[1], UINT64_C(777)) == ORES_NATIVE_SEND_ACCEPTED);

    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(atomic_load_explicit(&s.valid, memory_order_relaxed));
    assert(atomic_load_explicit(&s.seen[0], memory_order_relaxed) == 5);
    assert(atomic_load_explicit(&s.seen[1], memory_order_relaxed) == 3);

    // A callback finished: physical host carriers have no ambient data-mail
    // receipt. Old actor identities and foreign threads cannot recover it.
    uint64_t sentinel = UINT64_C(0xbeef);
    assert(!ores_native_actor_scheduler_current_data_mail_sequence(
        s.scheduler, s.actors[0], &sentinel));
    assert(sentinel == UINT64_C(0xbeef));
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[0]));
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[1]));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actors[0]));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actors[1]));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_ORIGINAL_DATA_MAIL_SEQUENCE_V1_PASS");
    return 0;
}
