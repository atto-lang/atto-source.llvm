#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVMMasterScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void check(int valid) { if (!valid) abort(); }
static void nap(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
typedef struct { atomic_int seen; } TestState;
static void actor_callback(void* ctx, size_t actor, uint64_t value) {
    TestState* state = (TestState*)ctx;
    check(actor == 0 && value == 42);
    atomic_fetch_add(&state->seen, 1);
}
typedef struct {
    OresNativeVMMasterScheduler* master;
    atomic_int successful, rejected;
} Producer;
static void* flood(void* raw) {
    Producer* p = (Producer*)raw;
    for (int i = 0; i < 2000; ++i) {
        if (ores_native_vm_master_notify(p->master, ORES_VM_POOL_SHARED))
            atomic_fetch_add(&p->successful, 1);
        else atomic_fetch_add(&p->rejected, 1);
    }
    return NULL;
}
int main(void) {
    TestState test = {0};
    OresNativeVMMasterScheduler* master = ores_native_vm_master_open();
    check(master != NULL);
    check(ores_native_vm_master_started_control_threads(master) == 1);
    OresNativeActorScheduler* pool =
        ores_native_actor_scheduler_open(2, 4, 8, actor_callback, &test);
    check(pool != NULL);
    /* A master with configured pools eagerly owns one thread; neither pool
     * scheduler nor actor carriers exist until first actor registration. */
    check(ores_native_actor_scheduler_started_control_threads(pool) == 0);
    check(ores_native_actor_scheduler_started_workers(pool) == 0);
    check(ores_native_vm_master_attach(master, ORES_VM_POOL_SHARED, pool));
    check(!ores_native_vm_master_attach(master, ORES_VM_POOL_PRIVATE, pool));
    check(!ores_native_vm_master_attach(master, ORES_VM_POOL_SHARED, pool));
    check(!ores_native_vm_master_notify(master, ORES_VM_POOL_PRIVATE));
    OresNativeActorRef ref = ores_native_actor_scheduler_spawn(pool, 0);
    check(ref.scheduler_domain != 0);
    check(ores_native_actor_scheduler_started_control_threads(pool) == 1);
    check(ores_native_actor_scheduler_started_workers(pool) == 0);
    check(ores_native_vm_master_notify(master, ORES_VM_POOL_SHARED));
    int delivered = 0;
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_vm_master_delivered_wakes(master) > 0) {
            delivered = 1; break;
        }
        nap();
    }
    check(delivered);
    /* Signalling CONTROL must not bootstrap actor workers or run callbacks. */
    check(ores_native_actor_scheduler_started_workers(pool) == 0);
    check(atomic_load(&test.seen) == 0);
    Producer producer = {0};
    producer.master = master;
    pthread_t writers[4];
    for (int i = 0; i < 4; i++)
        check(pthread_create(&writers[i], NULL, flood, &producer) == 0);
    /* Detach serializes against any master -> pool wake in progress. */
    check(ores_native_vm_master_detach(master, ORES_VM_POOL_SHARED, pool));
    for (int i = 0; i < 4; i++) check(pthread_join(writers[i], NULL) == 0);
    check(!ores_native_vm_master_notify(master, ORES_VM_POOL_SHARED));
    check(atomic_load(&producer.successful) +
          atomic_load(&producer.rejected) == 8000);
    check(ores_native_actor_scheduler_send(pool, ref, 42));
    check(ores_native_actor_scheduler_drain(pool));
    check(atomic_load(&test.seen) == 1);
    ores_native_actor_scheduler_retire(pool);
    check(ores_native_actor_scheduler_try_destroy(pool));
    check(!ores_native_vm_master_try_destroy(master)); /* still running */
    ores_native_vm_master_retire(master);
    check(!ores_native_vm_master_notify(master, ORES_VM_POOL_SHARED));
    check(ores_native_vm_master_try_destroy(master));
    puts("NATIVE_ORESVM_MASTER_PASS master=1 pool_scheduler=1 cold_workers=0 detach_fenced=1");
    return 0;
}
