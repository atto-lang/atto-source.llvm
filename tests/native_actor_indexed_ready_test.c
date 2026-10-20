#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum { ACTORS = 1024, ROUNDS = 4 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    atomic_int active[ACTORS], seen[ACTORS];
    atomic_int controls, errors;
} State;

static void check_actor(State* s, size_t slot) {
    if (slot >= ACTORS
        || !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[slot])
        || atomic_fetch_add(&s->active[slot], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
}
static void release_actor(State* s, size_t slot) {
    if (atomic_fetch_sub(&s->active[slot], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void on_data(void* context, size_t actor, uint64_t msg) {
    State* s = (State*)context;
    check_actor(s, actor);
    int ordinal = atomic_fetch_add(&s->seen[actor], 1);
    if (ordinal != (int)msg) atomic_fetch_add(&s->errors, 1);
    release_actor(s, actor);
}
static void on_control(void* context, size_t actor, uint64_t msg) {
    State* s = (State*)context;
    check_actor(s, actor);
    if (msg != UINT64_C(0xfacefeed)) atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->controls, 1);
    release_actor(s, actor);
}
int main(void) {
    /* Exact out-of-contract actor count must reject before allocation. */
    assert(!ores_native_actor_scheduler_open(65537, 1, 1, on_data, NULL));
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_control(
        ACTORS, ROUNDS + 2, 4, on_data, on_control, &s);
    assert(s.scheduler != NULL);
    for (size_t i = 0; i < ACTORS; i++) {
        s.refs[i] = ores_native_actor_scheduler_spawn(s.scheduler, i);
        assert(s.refs[i].scheduler_domain != 0);
    }
    /* Exercise all 16 occupancy words and the four transitions around
     * bit-word boundaries, including the final valid actor slot. */
    const size_t control_lanes[] = {0, 63, 64, 127, 128, 511, 512, 1023};
    for (int round = 0; round < ROUNDS; round++) {
        for (size_t i = 0; i < ACTORS; i++)
            assert(ores_native_actor_scheduler_send(
                s.scheduler, s.refs[i], (uint64_t)round));
        for (size_t k = 0; k < sizeof(control_lanes)/sizeof(control_lanes[0]); k++) {
            assert(ores_native_actor_scheduler_publish_control(
                s.scheduler, s.refs[control_lanes[k]], UINT64_C(0xfacefeed)));
        }
        assert(ores_native_actor_scheduler_drain(s.scheduler));
        assert(atomic_load(&s.controls) == (round + 1) * 8);
        assert(atomic_load(&s.errors) == 0);
    }
    for (size_t i = 0; i < ACTORS; i++) {
        assert(atomic_load(&s.seen[i]) == ROUNDS);
        assert(atomic_load(&s.active[i]) == 0);
        assert(ores_native_actor_scheduler_stop(s.scheduler, s.refs[i]));
        assert(ores_native_actor_scheduler_poll_done(s.scheduler, s.refs[i]));
    }
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_INDEXED_READY_PASS lanes=1024 control_boundaries=8 rounds=4 fifo=1 exclusive=1");
    return 0;
}
