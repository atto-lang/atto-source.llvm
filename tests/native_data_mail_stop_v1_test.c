#include "oreslang/runtime/NativeActorScheduler.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    _Atomic int stage;
} StopScenario;

static void on_data(void* opaque, size_t index, uint64_t payload) {
    StopScenario* s = (StopScenario*)opaque;
    assert(index == 0 && payload == UINT64_C(1234));
    uint64_t sequence = 0;
    assert(ores_native_actor_scheduler_current_data_mail_sequence(
        s->scheduler, s->actor, &sequence));
    assert(sequence == 1);
    atomic_store_explicit(&s->stage, 1, memory_order_release);
    while (atomic_load_explicit(&s->stage, memory_order_acquire) == 1)
        sched_yield();
    assert(atomic_load_explicit(&s->stage, memory_order_acquire) == 2);
    uint64_t untouched = UINT64_C(0xBEEF);
    assert(!ores_native_actor_scheduler_current_data_mail_sequence(
        s->scheduler, s->actor, &untouched));
    assert(untouched == UINT64_C(0xBEEF));
    atomic_store_explicit(&s->stage, 3, memory_order_release);
}

int main(void) {
    StopScenario s = {0};
    atomic_init(&s.stage, 0);
    s.scheduler = ores_native_actor_scheduler_open(1, 4, 1, on_data, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, UINT64_C(1234)) == ORES_NATIVE_SEND_ACCEPTED);

    struct timespec start, now;
    assert(timespec_get(&start, TIME_UTC) == TIME_UTC);
    while (atomic_load_explicit(&s.stage, memory_order_acquire) != 1) {
        assert(timespec_get(&now, TIME_UTC) == TIME_UTC);
        assert(now.tv_sec - start.tv_sec < 8);
        sched_yield();
    }
    // STOP linearizes while the physical DATA turn is still executing.
    // A previously valid original-mail receipt must now fail closed.
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    atomic_store_explicit(&s.stage, 2, memory_order_release);
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    assert(atomic_load_explicit(&s.stage, memory_order_acquire) == 3);
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_DATA_MAIL_STOP_REVOKES_RUNNING_TURN_PASS");
    return 0;
}
