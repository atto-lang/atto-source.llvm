#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum { SLOTS = 65536, ROUND_LANES = 4096, ROUNDS = 4 };
static OresNativeActorScheduler* scheduler;
static OresNativeActorRef refs[SLOTS];
static atomic_uint data_seen[SLOTS];
static atomic_uint control_seen[SLOTS];
static atomic_uint failures;

static void on_data(void* unused, size_t actor, uint64_t payload) {
    (void)unused;
    if (actor >= SLOTS || payload != actor
        || !ores_native_actor_scheduler_is_current_actor(scheduler, refs[actor])
        || atomic_fetch_add(&data_seen[actor], 1) != 0)
        atomic_fetch_add(&failures, 1);
}
static void on_control(void* unused, size_t actor, uint64_t payload) {
    (void)unused;
    if (actor >= SLOTS || payload != (uint64_t)(actor ^ UINT64_C(0xfedca987))
        || !ores_native_actor_scheduler_is_current_actor(scheduler, refs[actor])
        || atomic_fetch_add(&control_seen[actor], 1) != 0)
        atomic_fetch_add(&failures, 1);
}

int main(void) {
    scheduler = ores_native_actor_scheduler_open_control(
        SLOTS, 2, 8, on_data, on_control, NULL);
    assert(scheduler != NULL);
    /* Odd stride permutes the entire 2^16-bit actor index space; each round
     * activates 4,096 previously untouched actors, touching all 1,024
     * readiness words and every one of the 16 summary words. */
    for (size_t round = 0; round < ROUNDS; ++round) {
        for (size_t i = 0; i < ROUND_LANES; ++i) {
            size_t slot = (((round * ROUND_LANES + i) * 17) + 13) % SLOTS;
            refs[slot] = ores_native_actor_scheduler_spawn(scheduler, slot);
            assert(refs[slot].scheduler_domain != 0);
        }
        for (size_t i = 0; i < ROUND_LANES; ++i) {
            size_t slot = (((round * ROUND_LANES + i) * 17) + 13) % SLOTS;
            assert(ores_native_actor_scheduler_send(scheduler, refs[slot], slot));
            if (i % 64 == 0)
                assert(ores_native_actor_scheduler_publish_control(
                    scheduler, refs[slot], slot ^ UINT64_C(0xfedca987)));
        }
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(atomic_load(&failures) == 0);
        for (size_t i = 0; i < ROUND_LANES; ++i) {
            size_t slot = (((round * ROUND_LANES + i) * 17) + 13) % SLOTS;
            assert(atomic_load(&data_seen[slot]) == 1);
            assert(atomic_load(&control_seen[slot]) == (unsigned)(i % 64 == 0));
            assert(ores_native_actor_scheduler_stop(scheduler, refs[slot]));
            assert(ores_native_actor_scheduler_poll_done(scheduler, refs[slot]));
            assert(!ores_native_actor_scheduler_send(scheduler, refs[slot], slot));
            assert(!ores_native_actor_scheduler_stop(scheduler, refs[slot]));
        }
    }
    assert(ores_native_actor_scheduler_accepted(scheduler)
            == ROUNDS * (ROUND_LANES + ROUND_LANES / 64));
    assert(ores_native_actor_scheduler_completed(scheduler)
            == ROUNDS * (ROUND_LANES + ROUND_LANES / 64));
    ores_native_actor_scheduler_retire(scheduler);
    assert(ores_native_actor_scheduler_try_destroy(scheduler));
    puts("NATIVE_READY_SUMMARY_PASS actors=65536 distinct_turns=16384 controls=256 no_duplicates=1");
    return 0;
}
