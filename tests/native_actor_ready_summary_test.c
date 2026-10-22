#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
enum { ACTORS = 65536, SAMPLES = 512, ROUNDS = 16 };
typedef struct {
    OresNativeActorScheduler *scheduler;
    OresNativeActorRef *refs;
    atomic_uint *seen, *controls;
    atomic_int *active;
    atomic_int errors;
} State;
static void on_data(void *ctx, size_t actor, uint64_t value) {
    State *s = ctx;
    if (actor >= ACTORS || !ores_native_actor_scheduler_is_current_actor(
        s->scheduler, s->refs[actor])) abort();
    if (atomic_fetch_add(&s->active[actor], 1)) atomic_fetch_add(&s->errors, 1);
    if (atomic_fetch_add(&s->seen[actor], 1) != value) atomic_fetch_add(&s->errors, 1);
    if (atomic_fetch_sub(&s->active[actor], 1) != 1) atomic_fetch_add(&s->errors, 1);
}
static void on_control(void *ctx, size_t actor, uint64_t value) {
    State *s = ctx;
    if (actor >= ACTORS || value != UINT64_C(0xfeedcafe) ||
        !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor])) abort();
    if (atomic_fetch_add(&s->active[actor], 1)) atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->controls[actor], 1);
    if (atomic_fetch_sub(&s->active[actor], 1) != 1) atomic_fetch_add(&s->errors, 1);
}
int main(void) {
    State *s = calloc(1, sizeof(*s));
    assert(s);
    s->refs = calloc(ACTORS, sizeof(*s->refs));
    s->seen = calloc(ACTORS, sizeof(*s->seen));
    s->controls = calloc(ACTORS, sizeof(*s->controls));
    s->active = calloc(ACTORS, sizeof(*s->active));
    assert(s->refs && s->seen && s->controls && s->active);
    s->scheduler = ores_native_actor_scheduler_open_control(
        ACTORS, 3, 4, on_data, on_control, s);
    assert(s->scheduler);
    size_t selected[SAMPLES];
    selected[0] = ACTORS - 1;
    for (size_t i = 1; i < SAMPLES; ++i)
        selected[i] = ((i - 1) * 1009) % ACTORS;
    for (size_t i = 0; i < SAMPLES; ++i) {
        size_t actor = selected[i];
        s->refs[actor] = ores_native_actor_scheduler_spawn(s->scheduler, actor);
        assert(s->refs[actor].scheduler_domain);
    }
    for (uint64_t wave = 0; wave < ROUNDS; ++wave) {
        for (size_t i = 0; i < SAMPLES; ++i) {
            size_t actor = selected[i];
            assert(ores_native_actor_scheduler_send(s->scheduler, s->refs[actor], wave));
            if ((i % 4) == 0) assert(ores_native_actor_scheduler_publish_control(
                s->scheduler, s->refs[actor], UINT64_C(0xfeedcafe)));
        }
        assert(ores_native_actor_scheduler_drain(s->scheduler));
        assert(atomic_load(&s->errors) == 0);
        for (size_t i = 0; i < SAMPLES; ++i) {
            size_t actor = selected[i];
            assert(atomic_load(&s->seen[actor]) == wave + 1);
            if ((i % 4) == 0) assert(atomic_load(&s->controls[actor]) == wave + 1);
        }
    }
    for (size_t i = 0; i < SAMPLES; ++i) {
        size_t actor = selected[i];
        assert(ores_native_actor_scheduler_stop(s->scheduler, s->refs[actor]));
        assert(ores_native_actor_scheduler_poll_done(s->scheduler, s->refs[actor]));
    }
    ores_native_actor_scheduler_retire(s->scheduler);
    assert(ores_native_actor_scheduler_try_destroy(s->scheduler));
    free(s->active); free(s->controls); free(s->seen); free(s->refs); free(s);
    puts("NATIVE_READY_SUMMARY_PASS actors=65536 sparse=512 waves=16 fifo=1 control=1 exclusive=1");
    return 0;
}
