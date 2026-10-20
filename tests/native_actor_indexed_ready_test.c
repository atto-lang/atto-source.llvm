#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ACTORS = 4096, ROUNDS = 4 };
typedef struct {
    atomic_int* next;
    atomic_int* active;
    atomic_int errors, controls, total;
} State;
static void enter(State* s, size_t actor) {
    if (atomic_fetch_add(&s->active[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
}
static void leave(State* s, size_t actor) {
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void data(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    enter(s, actor);
    if (payload != (uint64_t)atomic_fetch_add(&s->next[actor], 1))
        atomic_fetch_add(&s->errors, 1);
    leave(s, actor);
    atomic_fetch_add(&s->total, 1);
}
static void control(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    enter(s, actor);
    if (payload != UINT64_C(0xdecafbad))
        atomic_fetch_add(&s->errors, 1);
    leave(s, actor);
    atomic_fetch_add(&s->controls, 1);
}
int main(void) {
    State state = {0};
    state.next = (atomic_int*)calloc(ACTORS, sizeof(atomic_int));
    state.active = (atomic_int*)calloc(ACTORS, sizeof(atomic_int));
    assert(state.next && state.active);
    for (size_t i = 0; i < ACTORS; i++) {
        atomic_init(&state.next[i], 0);
        atomic_init(&state.active[i], 0);
    }
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open_control(
        ACTORS, ROUNDS + 1, 8, data, control, &state);
    assert(s);
    OresNativeActorRef* refs = (OresNativeActorRef*)calloc(ACTORS, sizeof(*refs));
    assert(refs);
    for (size_t a = 0; a < ACTORS; a++) {
        refs[a] = ores_native_actor_scheduler_spawn(s, a);
        assert(refs[a].scheduler_domain != 0);
    }
    for (uint64_t round = 0; round < ROUNDS; round++)
        for (size_t a = 0; a < ACTORS; a++)
            assert(ores_native_actor_scheduler_send(s, refs[a], round));
    assert(ores_native_actor_scheduler_publish_control(
        s, refs[ACTORS - 1], UINT64_C(0xdecafbad)));
    assert(ores_native_actor_scheduler_drain(s));
    assert(ores_native_actor_scheduler_accepted(s) == ACTORS * ROUNDS + 1);
    assert(ores_native_actor_scheduler_completed(s) == ACTORS * ROUNDS + 1);
    assert(ores_native_actor_scheduler_control_completed(s) == 1);
    assert(atomic_load(&state.total) == ACTORS * ROUNDS);
    assert(atomic_load(&state.controls) == 1);
    assert(atomic_load(&state.errors) == 0);
    for (size_t a = 0; a < ACTORS; a++) {
        assert(atomic_load(&state.next[a]) == ROUNDS);
        assert(ores_native_actor_scheduler_stop(s, refs[a]));
        assert(ores_native_actor_scheduler_poll_done(s, refs[a]));
    }
    ores_native_actor_scheduler_retire(s);
    assert(ores_native_actor_scheduler_try_destroy(s));
    free(refs);
    free(state.next);
    free(state.active);

    /* Last bit of the last summary group; most actor lanes remain unspawned.
     * Verify no false hit, missing wakeup or out-of-bounds summary read. */
    State sparse = {0};
    sparse.next = (atomic_int*)calloc(65536, sizeof(atomic_int));
    sparse.active = (atomic_int*)calloc(65536, sizeof(atomic_int));
    assert(sparse.next && sparse.active);
    for (size_t a = 0; a < 65536; ++a) {
        atomic_init(&sparse.next[a], 0);
        atomic_init(&sparse.active[a], 0);
    }
    s = ores_native_actor_scheduler_open(65536, 1, 2, data, &sparse);
    assert(s);
    OresNativeActorRef last = ores_native_actor_scheduler_spawn(s, 65535);
    assert(last.scheduler_domain);
    assert(ores_native_actor_scheduler_send(s, last, 0));
    assert(ores_native_actor_scheduler_drain(s));
    assert(atomic_load(&sparse.total) == 1);
    assert(atomic_load(&sparse.errors) == 0);
    assert(ores_native_actor_scheduler_stop(s, last));
    ores_native_actor_scheduler_retire(s);
    assert(ores_native_actor_scheduler_try_destroy(s));
    free(sparse.next);
    free(sparse.active);
    assert(!ores_native_actor_scheduler_open(65537, 1, 1, data, NULL));
    puts("NATIVE_INDEXED_READY_PASS actors=4096 turns=16384 sparse_high_slot=65535");
    return 0;
}
