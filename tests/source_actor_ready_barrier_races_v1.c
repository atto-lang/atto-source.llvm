/*
 * Real production SourceActorBootstrap.c READY barrier regression oracle.
 * Three subprocess modes are run under ASan/UBSan by the exact-head shell
 * suite. No synthetic actor or fake callback is substituted for the VM.
 *
 * positive: 32 actual constructor/READY/physical-stop lifecycle cycles;
 * cancel:   stop while a distinct trusted host thread is waiting on the CV
 *           -> deliberate fail-stop SIGABRT, never a false READY;
 * carrier:  actor on_message attempting blocking host READY
 *           -> deliberate fail-stop SIGABRT before it can block.
 *
 * Requires ORESLANG_SOURCE_ACTOR_TESTING for the *observation* hooks.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int constructed;
static atomic_int host_wait_complete;
static atomic_int handler_entered;
static atomic_int handler_returned;
static _Atomic(void*) callback_actor;

static void check(int ok) {
    if (!ok) {
        fputs("SOURCE_ACTOR_READY_RACE_INVARIANT_FAILED\n", stderr);
        abort();
    }
}

static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    check(heap != NULL && domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(&constructed, 1, memory_order_acq_rel);
}

static int64_t on_message(OresActorI64Heap* heap, uint64_t domain,
                          uint64_t epoch, int64_t payload) {
    check(heap != NULL && domain != 0 && epoch != 0);
    if (payload == 99) {
        void* actor = atomic_load_explicit(
            &callback_actor, memory_order_acquire);
        check(actor != NULL);
        atomic_store_explicit(&handler_entered, 1, memory_order_release);
        /* This call is a semantic violation: it is executing ON the
         * actor's carrier and must be rejected before any pthread wait. */
        oreslang_source_actor_wait_ready_blocking(actor);
        atomic_store_explicit(&handler_returned, 1, memory_order_release);
    }
    return payload;
}

static void* host_waiter(void* actor) {
    oreslang_source_actor_wait_ready_blocking(actor);
    atomic_fetch_add_explicit(&host_wait_complete, 1, memory_order_acq_rel);
    return NULL;
}

static void* host_joiner(void* actor) {
    /* Independent trusted-host stop/join, concurrent with a pending
     * READY waiter. SourceActor must not be freed until that observer
     * revokes its lifetime pin under group.mutex. */
    oreslang_source_actor_join_done(actor);
    return NULL;
}

static void observe_held_initialization(void) {
    while (!oreslang_source_actor_test_initialization_entered())
        sched_yield();
}

static void observe_real_cv_wait(void) {
    while (!oreslang_source_actor_test_ready_waiter_blocked())
        sched_yield();
}

static void repeated_positive(int count) {
    for (int i = 0; i < count; ++i) {
        /* Deterministically keep constructor scheduling PENDING until
         * host_waiter has actually registered a CV wait. This tests the
         * missed-notification boundary, not merely pthread_create. */
        const int before_constructor = (i % 2) == 0;
        oreslang_source_actor_test_hold_initialization(before_constructor);
        oreslang_source_actor_test_hold_readiness_publication(
            !before_constructor);
        void* actor = oreslang_source_actor_spawn(initialize, on_message, 1);
        check(actor != NULL);
        if (before_constructor) {
            observe_held_initialization();
            check(oreslang_source_actor_ready_status(actor)
                  == ORES_SOURCE_ACTOR_STARTUP_PENDING);
        } else {
            /* Constructor has already completed, but READY cannot publish
             * until the separate test hook releases the group mutex race. */
            while (!oreslang_source_actor_test_readiness_publication_entered())
                sched_yield();
            check(oreslang_source_actor_ready_status(actor)
                  == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
        }
        pthread_t waiter;
        check(pthread_create(&waiter, NULL, host_waiter, actor) == 0);
        observe_real_cv_wait();
        check(atomic_load_explicit(&host_wait_complete,
                memory_order_acquire) == i);
        if (before_constructor)
            oreslang_source_actor_test_hold_initialization(0);
        else
            oreslang_source_actor_test_hold_readiness_publication(0);
        check(pthread_join(waiter, NULL) == 0);
        check(!oreslang_source_actor_test_ready_waiter_blocked());
        check(oreslang_source_actor_ready_status(actor)
              == ORES_SOURCE_ACTOR_STARTUP_READY);
        check(atomic_load_explicit(&host_wait_complete,
                memory_order_acquire) == i + 1);
        check(atomic_load_explicit(&constructed,
                memory_order_acquire) == i + 1);
        oreslang_source_actor_stop(actor);
    }
    /* READY can also win before the host arrives. The fast path must
     * observe the published state immediately, without parking on a stale
     * condition-variable wake or holding a no-longer-live group pointer. */
    for (int i = 0; i < 8; ++i) {
        void* actor = oreslang_source_actor_spawn(initialize, on_message, 1);
        check(actor != NULL);
        while (oreslang_source_actor_ready_status(actor)
                != ORES_SOURCE_ACTOR_STARTUP_READY)
            sched_yield();
        oreslang_source_actor_wait_ready_blocking(actor);
        check(!oreslang_source_actor_test_ready_waiter_blocked());
        oreslang_source_actor_stop(actor);
    }
    check(atomic_load_explicit(&constructed, memory_order_acquire)
          == count + 8);
    printf("SOURCE_ACTOR_READY_RACE_POSITIVE|constructors=%d|cv_waits=%d|fast_ready=8|joins=%d\n",
           atomic_load_explicit(&constructed, memory_order_acquire),
           atomic_load_explicit(&host_wait_complete, memory_order_acquire),
           count + 8);
}

static void cancelled_after_waiter_admission(int during_publication) {
    /* Two adversarial linearization windows:
     * (a) actor constructor still PENDING, and
     * (b) constructor has finished but publication is INITIALIZING.
     * Both are required to reject READY and preserve the waiting ref
     * while a second trusted host concurrently joins physical DONE. */
    oreslang_source_actor_test_hold_initialization(!during_publication);
    oreslang_source_actor_test_hold_readiness_publication(during_publication);
    void* actor = oreslang_source_actor_spawn(initialize, on_message, 1);
    check(actor != NULL);
    if (during_publication) {
        while (!oreslang_source_actor_test_readiness_publication_entered())
            sched_yield();
        check(atomic_load_explicit(&constructed, memory_order_acquire) == 1);
        check(oreslang_source_actor_ready_status(actor)
              == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
    } else {
        observe_held_initialization();
        check(oreslang_source_actor_ready_status(actor)
              == ORES_SOURCE_ACTOR_STARTUP_PENDING);
    }
    pthread_t waiter;
    check(pthread_create(&waiter, NULL, host_waiter, actor) == 0);
    observe_real_cv_wait();
    check(oreslang_source_actor_ready_status(actor)
          == (during_publication
              ? ORES_SOURCE_ACTOR_STARTUP_INITIALIZING
              : ORES_SOURCE_ACTOR_STARTUP_PENDING));
    oreslang_source_actor_request_stop(actor);
    pthread_t joiner;
    /* The adversarial host join is admitted while the observer owns a
     * lifetime pin. Neither READY-wait nor DONE-join may use freed memory. */
    check(pthread_create(&joiner, NULL, host_joiner, actor) == 0);
    if (during_publication)
        oreslang_source_actor_test_hold_readiness_publication(0);
    else
        oreslang_source_actor_test_hold_initialization(0);
    /* Cancellation must wake this admitted observer, revoke its pin,
     * and fail stop. A successful return or hang is a test failure. */
    check(pthread_join(waiter, NULL) == 0);
    check(pthread_join(joiner, NULL) == 0);
    fputs("UNEXPECTED_READY_AFTER_CANCEL\n", stderr);
    exit(9);
}

static void reject_actor_carrier(void) {
    void* actor = oreslang_source_actor_spawn(initialize, on_message, 1);
    check(actor != NULL);
    oreslang_source_actor_wait_ready_blocking(actor);
    atomic_store_explicit(&callback_actor, actor, memory_order_release);
    oreslang_source_actor_send(actor, 99);
    while (!atomic_load_explicit(&handler_entered, memory_order_acquire))
        sched_yield();
    while (!atomic_load_explicit(&handler_returned, memory_order_acquire))
        sched_yield();
    /* Reaching this line means a blocked guest carrier was wrongly admitted. */
    fputs("UNEXPECTED_READY_ON_ACTOR_CARRIER\n", stderr);
    exit(10);
}

int main(int argc, char** argv) {
    check(argc == 2);
    if (strcmp(argv[1], "positive") == 0) {
        repeated_positive(32);
        return 0;
    }
    if (strcmp(argv[1], "cancel-pending") == 0) {
        cancelled_after_waiter_admission(0);
        return 11;
    }
    if (strcmp(argv[1], "cancel-publishing") == 0) {
        cancelled_after_waiter_admission(1);
        return 11;
    }
    if (strcmp(argv[1], "carrier") == 0) {
        reject_actor_carrier();
        return 12;
    }
    return 64;
}
