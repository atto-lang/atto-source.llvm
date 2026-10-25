#include "oreslang/runtime/SourceActorBootstrap.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int constructor_calls;
static atomic_int constructor_entered;
static atomic_int constructor_release;
static atomic_int readiness_continuation_calls;
static atomic_int done_continuation_calls;

static void check(int condition) {
    if (!condition) abort();
}

static void constructor(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(
        &constructor_calls, 1, memory_order_acq_rel);
    atomic_store_explicit(
        &constructor_entered, 1, memory_order_release);
    while (!atomic_load_explicit(
            &constructor_release, memory_order_acquire))
        sched_yield();
}

static int64_t on_message(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    return payload;
}

static int64_t readiness_continuation(void* actor) {
    (void)actor;
    atomic_fetch_add_explicit(
        &readiness_continuation_calls, 1, memory_order_acq_rel);
    return 77;
}

static int64_t done_continuation(void* actor) {
    (void)actor;
    atomic_fetch_add_explicit(
        &done_continuation_calls, 1, memory_order_acq_rel);
    return 88;
}

static void check_failed_readiness(void* actor) {
    int64_t result = INT64_C(99);
    const int outcome = oreslang_source_actor_await_ready_checked(
        actor,
        readiness_continuation,
        UINT64_C(0x4242),
        &result);
    check(outcome
        == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    check(result == INT64_C(99));
    check(atomic_load_explicit(
        &readiness_continuation_calls, memory_order_acquire) == 0);
}

static void reset_constructor(void) {
    atomic_store_explicit(
        &constructor_calls, 0, memory_order_release);
    atomic_store_explicit(
        &constructor_entered, 0, memory_order_release);
    atomic_store_explicit(
        &constructor_release, 0, memory_order_release);
    atomic_store_explicit(
        &readiness_continuation_calls, 0, memory_order_release);
    atomic_store_explicit(
        &done_continuation_calls, 0, memory_order_release);
}

static void stop_before_constructor(void) {
    reset_constructor();
    oreslang_source_actor_test_hold_initialization(1);

    void* actor =
        oreslang_source_actor_spawn(constructor, on_message, 1);
    check(actor != NULL);

    while (!oreslang_source_actor_test_initialization_entered())
        sched_yield();

    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_PENDING);

    oreslang_source_actor_request_stop(actor);
    oreslang_source_actor_test_hold_initialization(0);

    while (oreslang_source_actor_ready_status(actor)
            == ORES_SOURCE_ACTOR_STARTUP_PENDING)
        sched_yield();

    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    check(atomic_load_explicit(
        &constructor_calls, memory_order_acquire) == 0);
    check_failed_readiness(actor);

    const int64_t done_result = oreslang_source_actor_await_done(
        actor, done_continuation, UINT64_C(0x6262));
    check(done_result == 88);
    check(atomic_load_explicit(
        &done_continuation_calls, memory_order_acquire) == 1);
}

static void stop_during_constructor(void) {
    reset_constructor();
    oreslang_source_actor_test_hold_initialization(0);
    oreslang_source_actor_test_hold_readiness_publication(0);

    void* actor =
        oreslang_source_actor_spawn(constructor, on_message, 1);
    check(actor != NULL);

    while (!atomic_load_explicit(
            &constructor_entered, memory_order_acquire))
        sched_yield();

    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);

    oreslang_source_actor_request_stop(actor);
    atomic_store_explicit(
        &constructor_release, 1, memory_order_release);

    while (oreslang_source_actor_ready_status(actor)
            == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING)
        sched_yield();

    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    check(atomic_load_explicit(
        &constructor_calls, memory_order_acquire) == 1);
    check_failed_readiness(actor);

    oreslang_source_actor_join_done(actor);
}

static void stop_after_constructor_before_ready(void) {
    reset_constructor();
    oreslang_source_actor_test_hold_initialization(0);
    oreslang_source_actor_test_hold_readiness_publication(1);
    atomic_store_explicit(
        &constructor_release, 1, memory_order_release);

    void* actor =
        oreslang_source_actor_spawn(constructor, on_message, 1);
    check(actor != NULL);

    while (!oreslang_source_actor_test_readiness_publication_entered())
        sched_yield();

    check(atomic_load_explicit(
        &constructor_calls, memory_order_acquire) == 1);
    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);

    /*
     * This is the lost-race window: guest construction has returned, but
     * readiness has not yet acquired the publication mutex.
     */
    oreslang_source_actor_request_stop(actor);
    oreslang_source_actor_test_hold_readiness_publication(0);

    while (oreslang_source_actor_ready_status(actor)
            == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING)
        sched_yield();

    check(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    check_failed_readiness(actor);
    oreslang_source_actor_join_done(actor);
}

static void successful_readiness(void) {
    reset_constructor();
    oreslang_source_actor_test_hold_initialization(0);
    atomic_store_explicit(
        &constructor_release, 1, memory_order_release);

    void* actor =
        oreslang_source_actor_spawn(constructor, on_message, 1);
    check(actor != NULL);

    while (oreslang_source_actor_ready_status(actor)
            != ORES_SOURCE_ACTOR_STARTUP_READY)
        sched_yield();

    int64_t result = 0;
    const int outcome = oreslang_source_actor_await_ready_checked(
        actor,
        readiness_continuation,
        UINT64_C(0x5151),
        &result);
    check(outcome == ORES_SOURCE_ACTOR_STARTUP_READY);
    check(result == 77);
    check(atomic_load_explicit(
        &readiness_continuation_calls, memory_order_acquire) == 1);
    check(atomic_load_explicit(
        &constructor_calls, memory_order_acquire) == 1);

    oreslang_source_actor_request_stop(actor);
    oreslang_source_actor_join_done(actor);
}

int main(void) {
    stop_before_constructor();
    stop_during_constructor();
    stop_after_constructor_before_ready();
    successful_readiness();
    puts("SOURCE_ACTOR_STOP_BEFORE_READY_PASS");
    return 0;
}
