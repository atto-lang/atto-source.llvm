#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Runs real compiler-ABI callbacks through SourceActorBootstrap and the
 * NativeOresVM scheduler, not the C++ NativeActorGroup test double. */
static atomic_int child_spawned, grandchild_spawned, grandchild_entered;
static atomic_int release_grandchild, stop_started, stop_finished;
static atomic_int grandchild_exited;
static void* top_actor;
static _Atomic(uintptr_t) spawned_child_handle;

static void check(int condition) { if (!condition) abort(); }

static void wait_for(atomic_int* flag) {
    /* Deadline-bounded test, never sleeps on an actor carrier. */
    for (int i = 0; i < 10000; ++i) {
        if (atomic_load_explicit(flag, memory_order_acquire)) return;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    abort();
}

static void init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain && epoch);
}

static int64_t grandchild_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 3);
    atomic_store_explicit(&grandchild_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&release_grandchild, memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&grandchild_exited, 1, memory_order_release);
    return 3;
}

static int64_t child_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 2);
    void* grandchild = oreslang_source_actor_spawn(
        init, grandchild_message, 1);
    check(grandchild != NULL);
    atomic_store_explicit(&grandchild_spawned, 1, memory_order_release);
    oreslang_source_actor_send(grandchild, 3);
    return 2;
}

static int64_t parent_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 1);
    void* child = oreslang_source_actor_spawn(init, child_message, 1);
    check(child != NULL);
    atomic_store_explicit(&spawned_child_handle,
        (uintptr_t)child, memory_order_release);
    atomic_store_explicit(&child_spawned, 1, memory_order_release);
    oreslang_source_actor_send(child, 2);
    return 1;
}

static atomic_int racing_parent_entered, allow_racing_spawn, racing_spawn_denied;

static int64_t parent_racing_spawn(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 9);
    atomic_store_explicit(&racing_parent_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&allow_racing_spawn, memory_order_acquire))
        sched_yield();
    /* The checked host ABI denies a spawn after this parent's STOP fence,
     * before consuming a slot. No child is created and no VM-wide abort. */
    void* denied = oreslang_source_actor_spawn(init, child_message, 1);
    check(denied == NULL);
    check(!oreslang_source_actor_try_send(top_actor, 99));
    atomic_store_explicit(&racing_spawn_denied, 1, memory_order_release);
    return 9;
}

static void* stop_on_trusted_host(void* opaque) {
    atomic_store_explicit(&stop_started, 1, memory_order_release);
    oreslang_source_actor_stop(opaque);
    atomic_store_explicit(&stop_finished, 1, memory_order_release);
    return NULL;
}

int main(void) {
    for (int round = 0; round < 10; ++round) {
        atomic_store(&child_spawned, 0);
        atomic_store(&spawned_child_handle, 0);
        atomic_store(&grandchild_spawned, 0);
        atomic_store(&grandchild_entered, 0);
        atomic_store(&release_grandchild, 0);
        atomic_store(&grandchild_exited, 0);
        atomic_store(&stop_started, 0);
        atomic_store(&stop_finished, 0);

        top_actor = oreslang_source_actor_spawn(init, parent_message, 1);
        check(top_actor != NULL);
        oreslang_source_actor_send(top_actor, 1);
        wait_for(&child_spawned);
        wait_for(&grandchild_spawned);
        wait_for(&grandchild_entered);
        /* Stop is called by a trusted host thread, never a carrier. */
        pthread_t closer;
        check(pthread_create(&closer, NULL, stop_on_trusted_host, top_actor) == 0);
        wait_for(&stop_started);
        int fenced = 0;
        for (int attempt = 0; attempt < 10000; ++attempt) {
            if (oreslang_source_actor_test_stopping(top_actor)) {
                fenced = 1;
                break;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        check(fenced);
        /* Concurrent overlapping STOPs must reject without double-
         * finalizing a child or turning the runtime into SIGABRT. */
        check(!oreslang_source_actor_try_stop(top_actor));
        void* live_child = (void*)(uintptr_t)atomic_load_explicit(
            &spawned_child_handle, memory_order_acquire);
        check(live_child != NULL);
        check(!oreslang_source_actor_try_stop(live_child));
        check(!atomic_load_explicit(&stop_finished, memory_order_acquire));
        atomic_store_explicit(&release_grandchild, 1, memory_order_release);
        check(pthread_join(closer, NULL) == 0);
        check(atomic_load(&stop_finished));
        check(atomic_load(&grandchild_exited));
    }
    /* Deterministic STOP-vs-spawn adversarial case. The host-only test
     * probe observes an actual committed fence, never just thread start. */
    for (int round = 0; round < 100; ++round) {
        atomic_store(&racing_parent_entered, 0);
        atomic_store(&allow_racing_spawn, 0);
        atomic_store(&racing_spawn_denied, 0);
        atomic_store(&stop_started, 0);
        atomic_store(&stop_finished, 0);
        void* parent = oreslang_source_actor_spawn(init, parent_racing_spawn, 1);
        check(parent != NULL);
        top_actor = parent;
        oreslang_source_actor_send(parent, 9);
        wait_for(&racing_parent_entered);
        pthread_t closer;
        check(pthread_create(&closer, NULL, stop_on_trusted_host, parent) == 0);
        wait_for(&stop_started);
        int fenced = 0;
        for (int i = 0; i < 10000; ++i) {
            if (oreslang_source_actor_test_stopping(parent)) {
                fenced = 1;
                break;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        check(fenced);
        check(!oreslang_source_actor_try_stop(parent));
        check(!atomic_load_explicit(&stop_finished, memory_order_acquire));
        atomic_store_explicit(&allow_racing_spawn, 1, memory_order_release);
        check(pthread_join(closer, NULL) == 0);
        check(atomic_load(&stop_finished));
        check(atomic_load(&racing_spawn_denied));
    }
    puts("SOURCE_ACTOR_IMPLICIT_CHILD_SCOPE_PASS depth3=10 stop_spawn=100 overlapping_stop=110");
    return 0;
}
