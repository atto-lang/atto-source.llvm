#include "oreslang/runtime/NativeActorScheduler.h"
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned init_calls;
static unsigned destroy_calls;
static unsigned create_calls;
static unsigned fail_init_at;
static unsigned fail_create_at;

extern int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern int __real_pthread_mutex_destroy(pthread_mutex_t*);
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                 void* (*)(void*), void*);

int __wrap_pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    if (++init_calls == fail_init_at) return EAGAIN;
    return __real_pthread_mutex_init(m, a);
}
int __wrap_pthread_mutex_destroy(pthread_mutex_t* m) {
    ++destroy_calls;
    return __real_pthread_mutex_destroy(m);
}
int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a,
                          void* (*fn)(void*), void* arg) {
    if (++create_calls == fail_create_at) return EAGAIN;
    return __real_pthread_create(t, a, fn, arg);
}
static void noop(void* ctx, size_t id, uint64_t value) {
    (void)ctx; (void)id; (void)value;
}
static void require(int ok, const char* msg) {
    if (!ok) { fputs(msg, stderr); fputc('\n', stderr); exit(1); }
}
static void scenario(unsigned bad_init, unsigned bad_create, unsigned expected_destroy) {
    init_calls = 0; destroy_calls = 0; create_calls = 0;
    fail_init_at = bad_init; fail_create_at = bad_create;
    OresNativeActorScheduler* scheduler = ores_native_actor_scheduler_open(2, 2, 2, noop, NULL);
    require(scheduler == NULL, "failure injection unexpectedly created scheduler");
    require(destroy_calls == expected_destroy,
            "startup path destroyed uninitialized mutex or leaked initialized mutex");
}
int main(void) {
    /* The scheduler mutex succeeds, but the publication gate init fails:
     * its storage must NOT be passed to pthread_mutex_destroy. */
    scenario(2, 0, 1);
    /* First carrier succeeds, second pthread_create fails. Both initialized
     * locks must be destroyed after retiring/joining the first worker. */
    scenario(0, 2, 2);
    /* First thread fails before any carrier exists. */
    scenario(0, 1, 2);
    puts("NATIVE_RESTART_STARTUP_FAULT_PASS init=1 worker_partial=1 worker_none=1");
    return 0;
}
