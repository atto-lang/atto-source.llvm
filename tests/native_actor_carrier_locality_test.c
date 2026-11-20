#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#if defined(NDEBUG)
#undef NDEBUG // Tests must not silently disappear in optimized CMake builds
#endif
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int first_entered, release_first;
    int seen[16], count;
} State;
static void on_message(void* ctx, size_t actor, uint64_t payload) {
    State* s = (State*)ctx;
    assert(pthread_mutex_lock(&s->mutex) == 0);
    assert(s->count < 16);
    s->seen[s->count++] = (int)(actor * 100 + payload);
    if (actor == 0 && payload == 0) {
        s->first_entered = 1;
        pthread_cond_broadcast(&s->cond);
        while (!s->release_first)
            assert(pthread_cond_wait(&s->cond, &s->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&s->mutex) == 0);
}
static void on_control(void* ctx, size_t actor, uint64_t payload) {
    State* s = (State*)ctx;
    assert(payload == 99);
    assert(pthread_mutex_lock(&s->mutex) == 0);
    assert(s->count < 16);
    s->seen[s->count++] = 900 + (int)actor;
    assert(pthread_mutex_unlock(&s->mutex) == 0);
}
int main(void) {
    State state = {0};
    assert(pthread_mutex_init(&state.mutex, NULL) == 0);
    assert(pthread_cond_init(&state.cond, NULL) == 0);
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_control(
            1, 8, 1, on_message, on_control, &state);
    assert(scheduler != NULL);
    OresNativeActorRef a = ores_native_actor_scheduler_spawn(scheduler, 0);
    assert(a.scheduler_domain);
    assert(ores_native_actor_scheduler_send(scheduler, a, 0));

    assert(pthread_mutex_lock(&state.mutex) == 0);
    while (!state.first_entered)
        assert(pthread_cond_wait(&state.cond, &state.mutex) == 0);
    assert(pthread_mutex_unlock(&state.mutex) == 0);

    /* The one physical actor lane is already active. More DATA and one
     * priority CONTROL are admitted without needing another OS carrier.
     * Callback order validates CONTROL precedence; DATA turns exercise
     * warm same-carrier reuse without granting thread-owned actor state. */
    assert(ores_native_actor_scheduler_send(scheduler, a, 1));
    assert(ores_native_actor_scheduler_send(scheduler, a, 2));
    assert(ores_native_actor_scheduler_send(scheduler, a, 3));
    assert(ores_native_actor_scheduler_publish_control(scheduler, a, 99));

    assert(pthread_mutex_lock(&state.mutex) == 0);
    state.release_first = 1;
    pthread_cond_broadcast(&state.cond);
    assert(pthread_mutex_unlock(&state.mutex) == 0);
    assert(ores_native_actor_scheduler_drain(scheduler));

    const int expected[] = {0, 900, 1, 2, 3};
    assert(state.count == 5);
    for (int i = 0; i < 5; ++i) assert(state.seen[i] == expected[i]);
    assert(ores_native_actor_scheduler_control_completed(scheduler) == 1);

    ores_native_actor_scheduler_retire(scheduler);
    assert(ores_native_actor_scheduler_try_destroy(scheduler));
    assert(pthread_cond_destroy(&state.cond) == 0);
    assert(pthread_mutex_destroy(&state.mutex) == 0);
    puts("native actor warm-cache reuse and CONTROL priority: OK");
    return 0;
}
