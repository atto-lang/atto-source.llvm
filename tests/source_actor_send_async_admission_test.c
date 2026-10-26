#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeOresVM.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) abort(); } while (0)

static atomic_int first_entered;
static atomic_int release_first;
static atomic_int block_first;
static atomic_uintptr_t carrier_probe_actor;
static atomic_int carrier_probe_status;
/* This foreign scheduler has a distinct domain from SourceActor's scheduler.
 * The carrier-side wait guard must not depend on scheduler pointer equality. */
static OresNativeActorScheduler* foreign_scheduler;
static OresNativeActorRef foreign_ref;
static atomic_int foreign_probe_status;

static void foreign_noop(void* ctx, size_t slot, uint64_t payload) {
    (void)ctx; (void)slot; (void)payload;
}

static int64_t foreign_wait_probe_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    CHECK(foreign_scheduler != NULL);
    const OresNativeSendStatus status = ores_native_actor_scheduler_wait_send(
        foreign_scheduler, foreign_ref, (uint64_t)payload);
    atomic_store_explicit(
        &foreign_probe_status, (int)status, memory_order_release);
    return payload;
}

static void init_actor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; (void)domain; (void)epoch;
}

static int64_t passthrough_message(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
    int64_t payload);

static int64_t blocking_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    if (atomic_exchange_explicit(&block_first, 0, memory_order_acq_rel)) {
        atomic_store_explicit(&first_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&release_first, memory_order_acquire))
            sched_yield();
    }
    return payload;
}

static int64_t carrier_probe_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    void* actor = (void*)atomic_load_explicit(
        &carrier_probe_actor, memory_order_acquire);
    CHECK(actor != NULL);
    const int status = oreslang_source_actor_await_send_async_checked(
        actor, payload + 1);
    atomic_store_explicit(
        &carrier_probe_status, status, memory_order_release);
    return payload;
}

typedef struct {
    void* actor;
    atomic_int started;
    atomic_int done;
    int status;
} AwaitThread;

static void* await_send(void* raw) {
    AwaitThread* state = (AwaitThread*)raw;
    atomic_store_explicit(&state->started, 1, memory_order_release);
    state->status =
        oreslang_source_actor_await_send_async_checked(state->actor, 999);
    atomic_store_explicit(&state->done, 1, memory_order_release);
    return NULL;
}

static int same_ref(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain != 0
        && a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot
        && a.generation == b.generation;
}

static void reset_blocker(void) {
    atomic_store_explicit(&first_entered, 0, memory_order_release);
    atomic_store_explicit(&release_first, 0, memory_order_release);
    atomic_store_explicit(&block_first, 1, memory_order_release);
}

static void wait_ready(void* actor) {
    for (;;) {
        const int status = oreslang_source_actor_ready_status(actor);
        if (status == ORES_SOURCE_ACTOR_STARTUP_READY) return;
        CHECK(status == ORES_SOURCE_ACTOR_STARTUP_PENDING
           || status == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
        sched_yield();
    }
}

static size_t saturate_real_mailbox(void* actor) {
    CHECK(oreslang_source_actor_try_send_checked(actor, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (!atomic_load_explicit(&first_entered, memory_order_acquire))
        sched_yield();

    size_t queued = 0;
    for (; queued < 1024; ++queued) {
        const int status =
            oreslang_source_actor_try_send_checked(actor, (int64_t)(queued + 2));
        if (status == ORES_NATIVE_SEND_MAILBOX_FULL) break;
        CHECK(status == ORES_NATIVE_SEND_ACCEPTED);
    }
    CHECK(queued > 0 && queued < 1024);
    CHECK(oreslang_source_actor_try_send_checked(actor, 777)
        == ORES_NATIVE_SEND_MAILBOX_FULL);
    return queued;
}

static void capacity_release_admits_waiter(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    OresNativeActorRef ref_a = {0}, ref_b = {0};
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_a));
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_b));
    CHECK(same_ref(ref_a, ref_b));

    const size_t queued = saturate_real_mailbox(actor);
    AwaitThread waiter = {.actor = actor, .status = -1};
    atomic_init(&waiter.started, 0);
    atomic_init(&waiter.done, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, await_send, &waiter) == 0);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 1)
        sched_yield();

    /* The scheduler-mutex snapshot proves a scheduler-owned Future is
     * genuinely PENDING while FULL. The blocked handler prevents capacity
     * from changing; no producer thread is required for Future progress. */
    CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(atomic_load_explicit(&waiter.done, memory_order_acquire));

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CAPACITY_PASS queued=%zu exact_ref=1\n", queued);
}

typedef struct {
    void* future;
    atomic_int started;
    int outcome;
} FutureObserverThread;

static void* observe_pending_future(void* raw) {
    FutureObserverThread* state = (FutureObserverThread*)raw;
    atomic_store_explicit(&state->started, 1, memory_order_release);
    state->outcome =
        oreslang_source_actor_await_send_future_checked(state->future);
    return NULL;
}

static void pending_future_await_excludes_concurrent_cancel(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 920, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    FutureObserverThread state = {.future = future, .outcome = -1};
    atomic_init(&state.started, 0);
    pthread_t observer;
    CHECK(pthread_create(&observer, NULL, observe_pending_future, &state)
        == 0);
    /* The test-only mutex snapshot ensures the Future has one live observer
     * before cancellation races it. Unlike sleeps, this is deterministic. */
    while (!oreslang_source_actor_test_future_consuming(future))
        sched_yield();
    CHECK(atomic_load_explicit(&state.started, memory_order_acquire));
    CHECK(!oreslang_source_actor_cancel_send_future(future));
    /* No double-release or premature descriptor invalidation is allowed.
     * The waiting host remains the unique owner until admission resolves. */
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(observer, NULL) == 0);
    CHECK(state.outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_AWAIT_CANCEL_RACE_PASS owner=1 cancel_rejected=1\n");
}

typedef struct {
    void* future;
    atomic_int* go;
    atomic_int* ready;
    int outcome;
} FutureRaceThread;

static void* race_observe_future(void* raw) {
    FutureRaceThread* state = (FutureRaceThread*)raw;
    atomic_fetch_add_explicit(state->ready, 1, memory_order_release);
    while (!atomic_load_explicit(state->go, memory_order_acquire))
        sched_yield();
    state->outcome =
        oreslang_source_actor_await_send_future_checked(state->future);
    return NULL;
}

static void pending_future_double_await_has_single_consumer(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 921, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    atomic_int go = ATOMIC_VAR_INIT(0);
    atomic_int ready = ATOMIC_VAR_INIT(0);
    FutureRaceThread first = {future, &go, &ready, -1};
    FutureRaceThread second = {future, &go, &ready, -1};
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, race_observe_future, &first) == 0);
    CHECK(pthread_create(&b, NULL, race_observe_future, &second) == 0);
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2)
        sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);

    while (!oreslang_source_actor_test_future_consuming(future))
        sched_yield();
    atomic_store_explicit(&release_first, 1, memory_order_release);

    CHECK(pthread_join(a, NULL) == 0);
    CHECK(pthread_join(b, NULL) == 0);
    const int accepted =
        (first.outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED)
        + (second.outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    const int rejected =
        (first.outcome == ORES_SOURCE_SEND_ASYNC_INVALID_REF)
        + (second.outcome == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(accepted == 1);
    CHECK(rejected == 1);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DOUBLE_AWAIT_PASS accepted=1 duplicate_rejected=1\n");
}

typedef struct {
    void* future;
    atomic_int started;
    int status;
    int64_t result;
} FutureContinuationThread;

static int64_t contention_resume_probe(void* actor) {
    CHECK(actor != NULL);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    return 4242;
}

static void* observe_future_with_continuation(void* raw) {
    FutureContinuationThread* state = (FutureContinuationThread*)raw;
    atomic_store_explicit(&state->started, 1, memory_order_release);
    state->status =
        oreslang_source_actor_await_send_future_continuation_checked(
            state->future,
            contention_resume_probe,
            UINT64_C(0x5ef0),
            &state->result);
    return NULL;
}

static void competing_root_send_continuation_fails_closed(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* first = NULL;
    void* second = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 922, &first) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 923, &second) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(first != NULL && second != NULL && first != second);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 2);

    FutureContinuationThread owner = {
        .future = first, .status = -1, .result = -1
    };
    atomic_init(&owner.started, 0);
    pthread_t thread;
    CHECK(pthread_create(
        &thread, NULL, observe_future_with_continuation, &owner) == 0);
    while (!oreslang_source_actor_test_future_consuming(first))
        sched_yield();
    CHECK(atomic_load_explicit(&owner.started, memory_order_acquire));

    int64_t refused_result = -1;
    CHECK(oreslang_source_actor_await_send_future_continuation_checked(
        second,
        contention_resume_probe,
        UINT64_C(0x5ef1),
        &refused_result) == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    CHECK(refused_result == 0);
    CHECK(!oreslang_source_actor_test_future_consuming(second));
    CHECK(oreslang_source_actor_cancel_send_future(second));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(owner.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(owner.result == 4242);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CONTINUATION_CONTENTION_PASS owner=1 refused=1 reusable_loser=1\n");
}

static void foreign_future_pointer_fails_closed_without_dereference(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    int foreign_storage = 0;
    void* foreign = &foreign_storage;
    CHECK(oreslang_source_actor_await_send_future_checked(foreign)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(!oreslang_source_actor_cancel_send_future(foreign));

    int64_t result = -1;
    CHECK(oreslang_source_actor_await_send_future_continuation_checked(
        foreign,
        contention_resume_probe,
        UINT64_C(0x5ef2),
        &result) == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(result == 0);

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_FOREIGN_FUTURE_PASS invalid_ref=1 no_deref=1\n");
}

static void stored_future_starts_before_await_and_progresses(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 901, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    /* Submission has returned to the caller while target admission remains
     * pending. Releasing target capacity progresses the Future independently
     * of any source await. */
    atomic_store_explicit(&release_first, 1, memory_order_release);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 0)
        sched_yield();

    CHECK(oreslang_source_actor_await_send_future_checked(future)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);
    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_FUTURE_PASS eager_start=1 independent_progress=1\n");
}

static void stale_source_future_pointer_never_aliases_new_future(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    void* first = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 911, &first) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(first != NULL);
    CHECK(oreslang_source_actor_await_send_future_checked(first)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    void* second = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 912, &second) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(second != NULL);
    CHECK(second != first);
    CHECK(oreslang_source_actor_await_send_future_checked(first)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(oreslang_source_actor_await_send_future_checked(second)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_STALE_HANDLE_PASS nonrecycled=1\n");
}

static atomic_int logical_send_resume_seen;

static int64_t stored_send_resume_probe(void* actor) {
    CHECK(actor != NULL);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    OresNativeExecutionHandle execution = {0, 0};
    CHECK(oreslang_source_actor_current_execution_handle(&execution));
    CHECK(ores_native_store_handle_valid(execution));
    CHECK(oreslang_source_actor_try_send_checked(actor, 913)
        == ORES_NATIVE_SEND_ACCEPTED);
    atomic_store_explicit(
        &logical_send_resume_seen, 1, memory_order_release);
    return 1234;
}

static void stored_future_await_resumes_fresh_root_continuation(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 912, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    atomic_store_explicit(
        &logical_send_resume_seen, 0, memory_order_release);

    const uint64_t host_waits_before =
        ores_native_vm_test_async_send_host_wait_calls();
    const int64_t result = oreslang_source_actor_await_send_future(
        future, stored_send_resume_probe, UINT64_C(0x5eed));
    CHECK(result == 1234);
    CHECK(ores_native_vm_test_async_send_host_wait_calls()
        == host_waits_before);
    CHECK(atomic_load_explicit(
        &logical_send_resume_seen, memory_order_acquire) == 1);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_LOGICAL_RESUME_PASS fresh_root_turn=1 trace_handle=1 host_settlement_observer=0\n");
}

static atomic_int detached_send_resume_seen;

static int64_t detached_send_resume_probe(void* actor) {
    CHECK(actor != NULL);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    OresNativeExecutionHandle execution = {0, 0};
    CHECK(oreslang_source_actor_current_execution_handle(&execution));
    CHECK(ores_native_store_handle_valid(execution));
    atomic_store_explicit(
        &detached_send_resume_seen, 1, memory_order_release);
    /* Compiler detachment lowers post-await STOP to nonblocking admission on
     * this resumed root turn. Physical join remains outer-host-only. */
    oreslang_source_actor_request_stop(actor);
    return 4321;
}

static void detached_future_releases_guest_stack_before_admission(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 915, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    atomic_store_explicit(
        &detached_send_resume_seen, 0, memory_order_release);

    const uint64_t host_waits_before =
        ores_native_vm_test_async_send_host_wait_calls();
    CHECK(oreslang_source_actor_detach_send_future_checked(
        future,
        detached_send_resume_probe,
        UINT64_C(0x5ef3)) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    /* Detach returns while mailbox admission is still physically impossible.
     * The original guest stack therefore cannot be the settlement waiter. */
    CHECK(oreslang_source_actor_test_future_consuming(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    CHECK(atomic_load_explicit(
        &detached_send_resume_seen, memory_order_acquire) == 0);
    CHECK(ores_native_vm_test_async_send_host_wait_calls()
        == host_waits_before);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    int64_t result = -1;
    CHECK(oreslang_source_actor_wait_detached_send_result(&result)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(result == 4321);
    CHECK(atomic_load_explicit(
        &detached_send_resume_seen, memory_order_acquire) == 1);
    CHECK(ores_native_vm_test_async_send_host_wait_calls()
        == host_waits_before);

    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DETACHED_STACK_PASS returned_while_pending=1 "
        "fresh_root_turn=1 final_join=1\n");
}

static atomic_int detached_state_resume_seen;

static int64_t detached_state_resume_probe(
        void* actor, const int64_t* state, uint64_t state_count) {
    CHECK(actor != NULL);
    CHECK(state != NULL);
    CHECK(state_count == 2);
    CHECK(state[0] == 40);
    CHECK(state[1] == 2);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    OresNativeExecutionHandle execution = {0, 0};
    CHECK(oreslang_source_actor_current_execution_handle(&execution));
    CHECK(ores_native_store_handle_valid(execution));
    atomic_store_explicit(
        &detached_state_resume_seen, 1, memory_order_release);
    oreslang_source_actor_request_stop(actor);
    return state[0] + 3;
}

static void detached_scalar_state_is_copied_before_guest_return(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 918, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    int64_t state[2] = {40, 2};
    atomic_store_explicit(
        &detached_state_resume_seen, 0, memory_order_release);
    const uint64_t host_waits_before =
        ores_native_vm_test_async_send_host_wait_calls();

    CHECK(oreslang_source_actor_detach_send_future_state_checked(
        future,
        detached_state_resume_probe,
        UINT64_C(0x5ef6),
        state,
        ORES_SOURCE_DETACHED_SCALAR_WORDS + 1)
        == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    CHECK(!oreslang_source_actor_test_future_consuming(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    CHECK(oreslang_source_actor_detach_send_future_state_checked(
        future,
        detached_state_resume_probe,
        UINT64_C(0x5ef6),
        state,
        2) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    /* The runtime must have copied both words before returning. Mutating the
     * caller's original stack buffer cannot alter the resumed state frame. */
    state[0] = 999;
    state[1] = 999;
    CHECK(oreslang_source_actor_test_future_consuming(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    CHECK(atomic_load_explicit(
        &detached_state_resume_seen, memory_order_acquire) == 0);
    CHECK(ores_native_vm_test_async_send_host_wait_calls()
        == host_waits_before);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    int64_t result = -1;
    CHECK(oreslang_source_actor_wait_detached_send_result(&result)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(result == 43);
    CHECK(atomic_load_explicit(
        &detached_state_resume_seen, memory_order_acquire) == 1);
    CHECK(ores_native_vm_test_async_send_host_wait_calls()
        == host_waits_before);

    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DETACHED_STATE_PASS copied=2 overbound_rejected=1 "
        "guest_stack_released=1 final_join=1\n");
}

static atomic_int detached_max_state_resume_seen;

static int64_t detached_max_state_resume_probe(
        void* actor, const int64_t* state, uint64_t state_count) {
    CHECK(actor != NULL);
    CHECK(state != NULL);
    CHECK(state_count == ORES_SOURCE_DETACHED_SCALAR_WORDS);
    for (uint64_t i = 0; i + 1 < state_count; ++i)
        CHECK(state[i] == (int64_t)(i + 1));
    CHECK(state[state_count - 1] == 42);
    atomic_store_explicit(
        &detached_max_state_resume_seen, 1, memory_order_release);
    oreslang_source_actor_request_stop(actor);
    return state[0] + state[state_count - 1];
}

static void detached_scalar_state_accepts_exact_bound(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 919, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    int64_t state[ORES_SOURCE_DETACHED_SCALAR_WORDS];
    for (uint64_t i = 0; i + 1 < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
        state[i] = (int64_t)(i + 1);
    state[ORES_SOURCE_DETACHED_SCALAR_WORDS - 1] = 42;
    atomic_store_explicit(
        &detached_max_state_resume_seen, 0, memory_order_release);

    CHECK(oreslang_source_actor_detach_send_future_state_checked(
        future,
        detached_max_state_resume_probe,
        UINT64_C(0x5ef7),
        NULL,
        1) == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    CHECK(!oreslang_source_actor_test_future_consuming(future));

    CHECK(oreslang_source_actor_detach_send_future_state_checked(
        future,
        detached_max_state_resume_probe,
        UINT64_C(0x5ef7),
        state,
        ORES_SOURCE_DETACHED_SCALAR_WORDS)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    for (uint64_t i = 0; i < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
        state[i] = -1;
    CHECK(oreslang_source_actor_test_future_consuming(future));

    atomic_store_explicit(&release_first, 1, memory_order_release);
    int64_t result = -1;
    CHECK(oreslang_source_actor_wait_detached_send_result(&result)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(result == 43);
    CHECK(atomic_load_explicit(
        &detached_max_state_resume_seen, memory_order_acquire) == 1);

    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DETACHED_STATE_BOUND_PASS words=16 null_state_rejected=1 copied=16 final_join=1\n");
}

typedef struct {
    atomic_int started;
    int status;
    int64_t result;
} DetachedWaitThread;

static void* wait_detached_result_thread(void* raw) {
    DetachedWaitThread* state = (DetachedWaitThread*)raw;
    atomic_store_explicit(&state->started, 1, memory_order_release);
    state->status =
        oreslang_source_actor_wait_detached_send_result(&state->result);
    return NULL;
}

static void detached_result_finalizer_is_single_consumer(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 917, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    atomic_store_explicit(
        &detached_send_resume_seen, 0, memory_order_release);

    CHECK(oreslang_source_actor_detach_send_future_checked(
        future,
        detached_send_resume_probe,
        UINT64_C(0x5ef5)) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    DetachedWaitThread first = {.status = -1, .result = -1};
    atomic_init(&first.started, 0);
    pthread_t first_waiter;
    CHECK(pthread_create(
        &first_waiter, NULL, wait_detached_result_thread, &first) == 0);
    while (!atomic_load_explicit(&first.started, memory_order_acquire))
        sched_yield();
    while (!oreslang_source_actor_test_detached_finalizer_claimed())
        sched_yield();

    int64_t duplicate_result = -1;
    CHECK(oreslang_source_actor_wait_detached_send_result(&duplicate_result)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(duplicate_result == 0);
    CHECK(oreslang_source_actor_test_future_consuming(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(first_waiter, NULL) == 0);
    CHECK(first.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(first.result == 4321);
    CHECK(atomic_load_explicit(
        &detached_send_resume_seen, memory_order_acquire) == 1);

    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DETACHED_FINALIZER_PASS single_consumer=1 duplicate_rejected=1 final_join=1\n");
}

static atomic_int teardown_resume_entered;
static atomic_int release_teardown_resume;

typedef struct {
    void* future;
    int status;
    int64_t result;
} TeardownFutureThread;

typedef struct {
    void* actor;
    atomic_int done;
} JoinActorThread;

static int64_t teardown_resume_probe(void* actor) {
    CHECK(actor != NULL);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    atomic_store_explicit(
        &teardown_resume_entered, 1, memory_order_release);
    while (!atomic_load_explicit(
            &release_teardown_resume, memory_order_acquire))
        sched_yield();
    return 5150;
}

static void* await_teardown_future(void* raw) {
    TeardownFutureThread* state = (TeardownFutureThread*)raw;
    state->status =
        oreslang_source_actor_await_send_future_continuation_checked(
            state->future,
            teardown_resume_probe,
            UINT64_C(0x5ef2),
            &state->result);
    return NULL;
}

static void* join_stopped_actor(void* raw) {
    JoinActorThread* state = (JoinActorThread*)raw;
    oreslang_source_actor_join_done(state->actor);
    atomic_store_explicit(&state->done, 1, memory_order_release);
    return NULL;
}

static void live_future_fences_actor_teardown_through_wake_exit(void) {
    atomic_store_explicit(
        &teardown_resume_entered, 0, memory_order_release);
    atomic_store_explicit(
        &release_teardown_resume, 0, memory_order_release);

    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 915, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);

    TeardownFutureThread awaiter = {
        .future = future, .status = -1, .result = -1
    };
    pthread_t await_thread;
    CHECK(pthread_create(
        &await_thread, NULL, await_teardown_future, &awaiter) == 0);

    while (!atomic_load_explicit(
            &teardown_resume_entered, memory_order_acquire))
        sched_yield();
    CHECK(oreslang_source_actor_test_future_consuming(future));

    /* Stop the target while its root continuation still owns the Future.
     * Physical target DONE may complete, but actor reclamation must remain
     * fenced by send_future_live until the wake callback exits and descriptor
     * release has physically retired the exact generation. */
    oreslang_source_actor_request_stop(actor);

    JoinActorThread joiner = {.actor = actor};
    atomic_init(&joiner.done, 0);
    pthread_t join_thread;
    CHECK(pthread_create(
        &join_thread, NULL, join_stopped_actor, &joiner) == 0);
    while (!oreslang_source_actor_test_join_waiting_on_futures(actor))
        sched_yield();

    CHECK(!atomic_load_explicit(&joiner.done, memory_order_acquire));
    CHECK(oreslang_source_actor_test_future_consuming(future));

    atomic_store_explicit(
        &release_teardown_resume, 1, memory_order_release);
    CHECK(pthread_join(await_thread, NULL) == 0);
    CHECK(awaiter.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(awaiter.result == 5150);

    CHECK(pthread_join(join_thread, NULL) == 0);
    CHECK(atomic_load_explicit(&joiner.done, memory_order_acquire));
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_TEARDOWN_FENCE_PASS live_future=1 consuming_until_resume=1 descriptor_release_after_wake=1 actor_reclaim_after_future=1\n");
}

static atomic_int failed_send_continuation_seen;

static int64_t failed_send_resume_probe(void* actor) {
    (void)actor;
    atomic_store_explicit(
        &failed_send_continuation_seen, 1, memory_order_release);
    return 9999;
}

static void failed_stored_future_resumes_only_for_trace_cleanup(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 914, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    atomic_store_explicit(
        &failed_send_continuation_seen, 0, memory_order_release);
    oreslang_source_actor_request_stop(actor);

    int64_t result = -1;
    CHECK(oreslang_source_actor_await_send_future_continuation_checked(
        future, failed_send_resume_probe, UINT64_C(0x5eef), &result)
        == ORES_SOURCE_SEND_ASYNC_TERMINATED);
    CHECK(result == 0);
    CHECK(atomic_load_explicit(
        &failed_send_continuation_seen, memory_order_acquire) == 0);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_join_done(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_FAILED_RESUME_PASS guest_continuation=0 trace_cleanup=1\n");
}

static void detached_pending_stop_skips_guest_and_joins(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 916, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    atomic_store_explicit(
        &failed_send_continuation_seen, 0, memory_order_release);

    CHECK(oreslang_source_actor_detach_send_future_checked(
        future,
        failed_send_resume_probe,
        UINT64_C(0x5ef4)) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    while (!oreslang_source_actor_test_future_consuming(future))
        sched_yield();

    /* STOP wins before mailbox admission. The detached continuation must
     * resume only for logical trace cleanup, never execute guest post-await
     * code, and outer-host wait must still own physical reclamation. */
    oreslang_source_actor_request_stop(actor);
    atomic_store_explicit(&release_first, 1, memory_order_release);

    int64_t result = -1;
    CHECK(oreslang_source_actor_wait_detached_send_result(&result)
        == ORES_SOURCE_SEND_ASYNC_TERMINATED);
    CHECK(result == 0);
    CHECK(atomic_load_explicit(
        &failed_send_continuation_seen, memory_order_acquire) == 0);

    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DETACHED_STOP_PASS terminated=1 "
        "guest_continuation=0 trace_cleanup=1 final_join=1\n");
}

static void discard_started_future_consumes_pending_and_ready(void) {
    void* ready_actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(ready_actor != NULL);
    wait_ready(ready_actor);

    void* ready_future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        ready_actor, 930, &ready_future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(ready_future != NULL);
    CHECK(oreslang_source_actor_discard_send_future_checked(ready_future)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(oreslang_source_actor_await_send_future_checked(ready_future)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    oreslang_source_actor_stop(ready_actor);

    reset_blocker();
    void* pending_actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(pending_actor != NULL);
    wait_ready(pending_actor);
    (void)saturate_real_mailbox(pending_actor);

    void* pending_future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        pending_actor, 931, &pending_future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(pending_future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(pending_actor) == 1);
    CHECK(oreslang_source_actor_discard_send_future_checked(pending_future)
        == ORES_SOURCE_SEND_ASYNC_CANCELLED);
    CHECK(oreslang_source_actor_test_pending_async_sends(pending_actor) == 0);
    CHECK(oreslang_source_actor_await_send_future_checked(pending_future)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_stop(pending_actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DISCARD_RACE_PASS ready=accepted pending=cancelled consumed=1\n");
}

static void stored_future_cancel_wins_only_while_pending(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 902, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    CHECK(oreslang_source_actor_cancel_send_future(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_CANCEL_PASS pending_only=1\n");
}

static void stop_wakes_full_waiter_as_terminated(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    AwaitThread waiter = {.actor = actor, .status = -1};
    atomic_init(&waiter.started, 0);
    atomic_init(&waiter.done, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, await_send, &waiter) == 0);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 1)
        sched_yield();
    CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));

    /* Sealed STOP is itself a scheduler credit-state transition. The waiter
     * must wake and observe TERMINATED even though the active turn is still
     * physically blocked and cannot return ordinary mailbox capacity yet. */
    oreslang_source_actor_request_stop(actor);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.status == ORES_SOURCE_SEND_ASYNC_TERMINATED);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_join_done(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_STOP_PASS terminated=1\n");
}

static int64_t passthrough_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    return payload;
}

static void rejected_ready_cancel_releases_ownership_claim(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 921, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    /* READY admissions cannot be cancelled, but a rejected cancellation
     * must surrender its transient ownership so the later await succeeds. */
    CHECK(!oreslang_source_actor_cancel_send_future(future));
    CHECK(!oreslang_source_actor_test_future_consuming(future));
    CHECK(oreslang_source_actor_await_send_future_checked(future)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_READY_CANCEL_PASS claim_released=1 await=1\n");
}

static void bounded_waiter_cohort_rejects_before_pin(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    CHECK(oreslang_source_actor_test_set_async_waiters(
        actor, ORES_SOURCE_SEND_ASYNC_MAX_WAITERS));
    CHECK(oreslang_source_actor_await_send_async_checked(actor, 7)
        == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    CHECK(oreslang_source_actor_test_set_async_waiters(actor, 0));

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_COHORT_PASS max_waiters=%d resource_exhausted=1\n",
        ORES_SOURCE_SEND_ASYNC_MAX_WAITERS);
}

static void carrier_call_fails_would_deadlock(void) {
    atomic_store_explicit(&carrier_probe_actor, (uintptr_t)0, memory_order_release);
    atomic_store_explicit(&carrier_probe_status, -1, memory_order_release);
    void* actor = oreslang_source_actor_spawn(
        init_actor, carrier_probe_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    atomic_store_explicit(
        &carrier_probe_actor, (uintptr_t)actor, memory_order_release);

    CHECK(oreslang_source_actor_try_send_checked(actor, 5)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (atomic_load_explicit(
            &carrier_probe_status, memory_order_acquire) == -1)
        sched_yield();

    CHECK(atomic_load_explicit(
        &carrier_probe_status, memory_order_acquire)
        == ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK);
    oreslang_source_actor_stop(actor);
    atomic_store_explicit(
        &carrier_probe_actor, (uintptr_t)0, memory_order_release);
    fprintf(stderr, "SOURCE_SEND_ASYNC_DEADLOCK_PASS carrier_rejected=1\n");
}

static void foreign_carrier_probe(void* raw, size_t slot, uint64_t payload) {
    (void)slot;
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    const int status = oreslang_source_actor_await_send_async_checked(
        raw, (int64_t)payload);
    atomic_store_explicit(&carrier_probe_status, status, memory_order_release);
}

static void foreign_scheduler_carrier_fails_would_deadlock(void) {
    void* actor = oreslang_source_actor_spawn(init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    atomic_store_explicit(&carrier_probe_status, -1, memory_order_release);
    OresNativeActorScheduler* foreign = ores_native_actor_scheduler_open(
        1, 1, 1, foreign_carrier_probe, actor);
    CHECK(foreign);
    OresNativeActorRef producer = ores_native_actor_scheduler_spawn(foreign, 0);
    CHECK(ores_native_actor_scheduler_send(foreign, producer, 17));
    CHECK(ores_native_actor_scheduler_drain(foreign));
    CHECK(atomic_load_explicit(&carrier_probe_status, memory_order_acquire)
        == ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK);
    CHECK(!ores_native_actor_scheduler_is_carrier_thread());
    CHECK(ores_native_actor_scheduler_stop(foreign, producer));
    ores_native_actor_scheduler_retire(foreign);
    CHECK(ores_native_actor_scheduler_try_destroy(foreign));
    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_FOREIGN_CARRIER_PASS rejected=1\n");
}

static void cross_scheduler_carrier_cannot_park(void) {
    atomic_store_explicit(&foreign_probe_status, -1, memory_order_release);
    foreign_scheduler = ores_native_actor_scheduler_open(
        1, 1, 1, foreign_noop, NULL);
    CHECK(foreign_scheduler != NULL);
    foreign_ref = ores_native_actor_scheduler_spawn(foreign_scheduler, 0);
    CHECK(foreign_ref.scheduler_domain != 0);

    void* actor = oreslang_source_actor_spawn(
        init_actor, foreign_wait_probe_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    OresNativeActorRef source_ref = {0};
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &source_ref));
    CHECK(source_ref.scheduler_domain != foreign_ref.scheduler_domain);

    CHECK(oreslang_source_actor_try_send_checked(actor, 83)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (atomic_load_explicit(
            &foreign_probe_status, memory_order_acquire) == -1)
        sched_yield();
    /* Previously executing != foreign_scheduler permitted blocking here.
     * The API is external-host-only even when the target has free slots. */
    CHECK(atomic_load_explicit(
        &foreign_probe_status, memory_order_acquire)
        == ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE);
    oreslang_source_actor_stop(actor);

    CHECK(ores_native_actor_scheduler_stop(
        foreign_scheduler, foreign_ref));
    CHECK(ores_native_actor_scheduler_wait_done(
        foreign_scheduler, foreign_ref));
    ores_native_actor_scheduler_retire(foreign_scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(foreign_scheduler));
    foreign_scheduler = NULL;
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CROSS_SCHEDULER_DEADLOCK_PASS rejected=1\n");
}

int main(void) {
    atomic_init(&first_entered, 0);
    atomic_init(&release_first, 0);
    atomic_init(&block_first, 0);
    atomic_init(&carrier_probe_actor, (uintptr_t)0);
    atomic_init(&carrier_probe_status, -1);
    atomic_init(&foreign_probe_status, -1);
    atomic_init(&logical_send_resume_seen, 0);
    atomic_init(&detached_send_resume_seen, 0);
    atomic_init(&detached_state_resume_seen, 0);
    atomic_init(&detached_max_state_resume_seen, 0);
    atomic_init(&teardown_resume_entered, 0);
    atomic_init(&release_teardown_resume, 0);
    atomic_init(&failed_send_continuation_seen, 0);

    capacity_release_admits_waiter();
    stored_future_starts_before_await_and_progresses();
    pending_future_await_excludes_concurrent_cancel();
    pending_future_double_await_has_single_consumer();
    competing_root_send_continuation_fails_closed();
    foreign_future_pointer_fails_closed_without_dereference();
    stale_source_future_pointer_never_aliases_new_future();
    stored_future_await_resumes_fresh_root_continuation();
    detached_future_releases_guest_stack_before_admission();
    detached_scalar_state_is_copied_before_guest_return();
    detached_scalar_state_accepts_exact_bound();
    detached_result_finalizer_is_single_consumer();
    live_future_fences_actor_teardown_through_wake_exit();
    failed_stored_future_resumes_only_for_trace_cleanup();
    detached_pending_stop_skips_guest_and_joins();
    discard_started_future_consumes_pending_and_ready();
    stored_future_cancel_wins_only_while_pending();
    stop_wakes_full_waiter_as_terminated();
    rejected_ready_cancel_releases_ownership_claim();
    bounded_waiter_cohort_rejects_before_pin();
    carrier_call_fails_would_deadlock();
    foreign_scheduler_carrier_fails_would_deadlock();
    cross_scheduler_carrier_cannot_park();
    return 0;
}
