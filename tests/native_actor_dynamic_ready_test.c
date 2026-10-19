#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAX_ACTORS = 65536 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef* refs;
    atomic_int active[MAX_ACTORS > 0 ? 1 : -1]; /* diagnostic slot is unused */
    atomic_int seen;
    atomic_int errors;
    atomic_int controls;
    size_t previous_data[10];
    size_t chosen[10];
    size_t count;
} State;

static void on_data(void* ptr, size_t actor, uint64_t message) {
    State* s = (State*)ptr;
    if (actor >= MAX_ACTORS || !ores_native_actor_scheduler_is_current_actor(
            s->scheduler, s->refs[actor]))
        atomic_fetch_add(&s->errors, 1);
    int matched = 0;
    for (size_t j = 0; j < s->count; ++j) {
        if (s->chosen[j] != actor) continue;
        if (s->previous_data[j] != (size_t)message)
            atomic_fetch_add(&s->errors, 1);
        ++s->previous_data[j];
        matched = 1;
        break;
    }
    if (!matched) atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->seen, 1);
}
static void on_control(void* ptr, size_t actor, uint64_t message) {
    State* s = (State*)ptr;
    if (actor >= MAX_ACTORS || !ores_native_actor_scheduler_is_current_actor(
            s->scheduler, s->refs[actor]) || message != UINT64_C(0xc001cafe))
        atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->controls, 1);
}
static void exercise(size_t actors) {
    State* s = (State*)calloc(1, sizeof(*s));
    assert(s);
    s->refs = (OresNativeActorRef*)calloc(actors, sizeof(OresNativeActorRef));
    assert(s->refs);
    s->scheduler = ores_native_actor_scheduler_open_control(
        actors, 3, 4, on_data, on_control, s);
    assert(s->scheduler);
    const size_t targets[] = {0, 63, 64, 1023, 1024, 4095, 4096, 16383, 16384, 65535};
    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); ++i) {
        if (targets[i] >= actors) continue;
        s->chosen[s->count++] = targets[i];
        s->refs[targets[i]] = ores_native_actor_scheduler_spawn(s->scheduler, targets[i]);
        assert(s->refs[targets[i]].scheduler_domain);
    }
    for (int round = 0; round < 2; ++round) {
        for (size_t i = 0; i < s->count; ++i) {
            const size_t actor = s->chosen[i];
            assert(ores_native_actor_scheduler_send(s->scheduler, s->refs[actor],
                (uint64_t)round));
            assert(ores_native_actor_scheduler_publish_control(
                s->scheduler, s->refs[actor], UINT64_C(0xc001cafe)));
        }
        assert(ores_native_actor_scheduler_drain(s->scheduler));
        assert(atomic_load(&s->errors) == 0);
        assert(atomic_load(&s->controls) == (round + 1) * (int)s->count);
    }
    assert(atomic_load(&s->seen) == 2 * (int)s->count);
    for (size_t i = 0; i < s->count; ++i) {
        size_t actor = s->chosen[i];
        assert(s->previous_data[i] == 2);
        assert(ores_native_actor_scheduler_stop(s->scheduler, s->refs[actor]));
        assert(ores_native_actor_scheduler_poll_done(s->scheduler, s->refs[actor]));
    }
    ores_native_actor_scheduler_retire(s->scheduler);
    assert(ores_native_actor_scheduler_try_destroy(s->scheduler));
    free(s->refs);
    free(s);
}
int main(void) {
    assert(!ores_native_actor_scheduler_open(
        MAX_ACTORS + 1, 1, 1, on_data, NULL));
    assert(!ores_native_actor_scheduler_open(
        MAX_ACTORS, 17, 1, on_data, NULL)); /* 1,114,112 mailbox words */
    assert(!ores_native_actor_scheduler_open(
        1025, 1024, 1, on_data, NULL)); /* exceeds new large-pool budget */
    exercise(1025);
    exercise(4097);
    exercise(16385);
    exercise(MAX_ACTORS);
    puts("NATIVE_DYNAMIC_READY_PASS 1025,4097,16385,65536 actor lanes and resource admission");
    return 0;
}
