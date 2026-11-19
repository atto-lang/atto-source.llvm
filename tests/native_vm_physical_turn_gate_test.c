#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#if defined(NDEBUG)
#undef NDEBUG // Tests must not silently disappear in optimized CMake builds
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int entered_first, unblock_first;
    _Atomic int active, peak, entered_other, completed;
} State;

typedef struct { State* state; int domain; } DomainContext;

static void on_message(void* raw, size_t actor, uint64_t value) {
    DomainContext* ctx = (DomainContext*)raw;
    State* s = ctx->state;
    assert(actor == 0 && value == 7);
    int count = atomic_fetch_add(&s->active, 1) + 1;
    int observed = atomic_load(&s->peak);
    while (count > observed &&
           !atomic_compare_exchange_weak(&s->peak, &observed, count)) { }
    if (ctx->domain == 0) {
        assert(pthread_mutex_lock(&s->lock) == 0);
        s->entered_first = 1;
        pthread_cond_broadcast(&s->changed);
        while (!s->unblock_first)
            assert(pthread_cond_wait(&s->changed, &s->lock) == 0);
        assert(pthread_mutex_unlock(&s->lock) == 0);
    } else {
        atomic_fetch_add(&s->entered_other, 1);
    }
    atomic_fetch_sub(&s->active, 1);
    atomic_fetch_add(&s->completed, 1);
}

int main(void) {
    assert(ores_native_vm_open_with_physical_turn_limit(NULL, 1) == NULL);
    State s = {0};
    assert(pthread_mutex_init(&s.lock, NULL) == 0);
    assert(pthread_cond_init(&s.changed, NULL) == 0);
    DomainContext contexts[ORES_NATIVE_VM_DOMAIN_COUNT] = {{0}};
    OresNativeVMConfig config = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        contexts[i].state = &s;
        contexts[i].domain = i;
        config.domains[i].actor_slots = 1;
        config.domains[i].mailbox_capacity = 2;
        config.domains[i].carrier_workers = 2;
        config.domains[i].on_message = on_message;
        config.domains[i].context = &contexts[i];
    }
    OresNativeVM* vm = ores_native_vm_open_with_physical_turn_limit(&config, 1);
    assert(vm != NULL);
    OresNativeActorRef main_actor = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 0);
    OresNativeActorRef other = ores_native_vm_spawn(vm, ORES_NATIVE_VM_SHARED, 0);
    assert(main_actor.scheduler_domain && other.scheduler_domain);
    assert(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, main_actor, 7)
           == ORES_NATIVE_SEND_ACCEPTED);

    assert(pthread_mutex_lock(&s.lock) == 0);
    while (!s.entered_first)
        assert(pthread_cond_wait(&s.changed, &s.lock) == 0);
    assert(pthread_mutex_unlock(&s.lock) == 0);
    assert(ores_native_vm_active_physical_turns(vm) == 1);

    assert(ores_native_vm_try_send(vm, ORES_NATIVE_VM_SHARED, other, 7)
           == ORES_NATIVE_SEND_ACCEPTED);
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
    assert(atomic_load(&s.entered_other) == 0);
    assert(ores_native_vm_active_physical_turns(vm) == 1);

    assert(pthread_mutex_lock(&s.lock) == 0);
    s.unblock_first = 1;
    pthread_cond_broadcast(&s.changed);
    assert(pthread_mutex_unlock(&s.lock) == 0);

    /* Retirement with an admitted turn waiting on another domain's permit
     * must wake and drain both callbacks. No gate pointer survives teardown. */
    assert(ores_native_vm_close(vm));
    assert(atomic_load(&s.completed) == 2);
    assert(atomic_load(&s.peak) == 1);
    assert(atomic_load(&s.active) == 0);
    assert(pthread_cond_destroy(&s.changed) == 0);
    assert(pthread_mutex_destroy(&s.lock) == 0);
    puts("native cross-domain nonparking actor turn cap and retirement: OK");
    return 0;
}
