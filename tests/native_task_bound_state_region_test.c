#include "oreslang/runtime/OresTaskBoundStateRegion.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        abort(); \
    } \
} while (0)

static ores_state_task_identity task(
        uint64_t runtime_domain, uint64_t execution,
        uint32_t slot, uint32_t generation) {
    ores_state_task_identity out = {
        .logical={runtime_domain, execution},
        .handle={slot, generation}
    };
    return out;
}

static ores_std_sync_result request_retry(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_access access) {
    ores_std_sync_result out;
    do { out = ores_state_region_request(region, owner, access); }
    while (out.code == ORES_STD_SYNC_RETRY);
    CHECK(out.code == ORES_STD_SYNC_GRANTED ||
          out.code == ORES_STD_SYNC_QUEUED);
    return out;
}

static void start_retry(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    for (;;) {
        ores_std_sync_code code = ores_state_region_start(region, owner, ticket);
        if (code == ORES_STD_SYNC_GRANTED) return;
        CHECK(code == ORES_STD_SYNC_RETRY || code == ORES_STD_SYNC_QUEUED);
        sched_yield();
    }
}

static void end_retry(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    ores_std_sync_code code;
    do { code = ores_state_region_end(region, owner, ticket); }
    while (code == ORES_STD_SYNC_RETRY);
    CHECK(code == ORES_STD_SYNC_GRANTED);
}

static ores_std_sync_result release_retry(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    ores_std_sync_result out;
    do { out = ores_state_region_release(region, owner, ticket); }
    while (out.code == ORES_STD_SYNC_RETRY);
    return out;
}

static void test_execution_store_authentication(void) {
    OresNativeExecutionContextStore* store =
        ores_native_execution_context_store_open(4u);
    CHECK(store != NULL);

    OresNativeExecutionContext context = {0};
    context.id.runtime_domain = 44u;
    context.id.execution = 7001u;
    OresNativeExecutionHandle handle = {0};
    CHECK(ores_native_execution_context_store_insert(
        store, &context, &handle) != 0);

    ores_state_task_identity authenticated = {0};
    CHECK(ores_state_task_identity_from_store(
        store, handle, context.id, &authenticated));
    CHECK(authenticated.logical.runtime_domain == 44u);
    CHECK(authenticated.logical.execution == 7001u);
    CHECK(authenticated.handle.slot == handle.slot);
    CHECK(authenticated.handle.generation == handle.generation);

    OresNativeExecutionId wrong = {44u, 7002u};
    CHECK(!ores_state_task_identity_from_store(
        store, handle, wrong, &authenticated));

    OresNativeExecutionHandle stale = handle;
    ++stale.generation;
    CHECK(!ores_state_task_identity_from_store(
        store, stale, context.id, &authenticated));

    CHECK(ores_native_execution_context_store_release(store, handle) != 0);
    CHECK(!ores_state_task_identity_from_store(
        store, handle, context.id, &authenticated));
    CHECK(ores_native_execution_context_store_close(store) != 0);
}

static void test_identity_and_stale_release(void) {
    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_INIT(77u, 9001u);
    const ores_state_task_identity owner = task(1u, 10u, 3u, 5u);
    const ores_state_task_identity other_task = task(1u, 11u, 4u, 5u);
    const ores_state_task_identity stale_handle = task(1u, 10u, 3u, 4u);
    const ores_state_task_identity wrong_domain = task(2u, 10u, 3u, 5u);

    ores_std_sync_result ticket =
        request_retry(&region, owner, ORES_STD_WRITE);
    CHECK(ticket.code == ORES_STD_SYNC_GRANTED);

    CHECK(ores_state_region_start(&region, other_task, ticket.ticket) ==
          ORES_STD_SYNC_INVALID);
    CHECK(ores_state_region_start(&region, stale_handle, ticket.ticket) ==
          ORES_STD_SYNC_INVALID);
    CHECK(ores_state_region_start(&region, wrong_domain, ticket.ticket) ==
          ORES_STD_SYNC_INVALID);

    start_retry(&region, owner, ticket.ticket);
    CHECK(ores_state_region_release(&region, other_task, ticket.ticket).code ==
          ORES_STD_SYNC_INVALID);
    CHECK(ores_state_region_release(&region, owner, ticket.ticket).code ==
          ORES_STD_SYNC_EXECUTING);
    CHECK(ores_state_region_ready_to_suspend(&region, owner, ticket.ticket) ==
          ORES_STD_SYNC_EXECUTING);

    end_retry(&region, owner, ticket.ticket);
    CHECK(release_retry(&region, owner, ticket.ticket).code ==
          ORES_STD_SYNC_GRANTED);
    CHECK(ores_state_region_release(&region, owner, ticket.ticket).code ==
          ORES_STD_SYNC_STALE);
    CHECK(ores_state_region_start(&region, owner, ticket.ticket) ==
          ORES_STD_SYNC_STALE);
}

static void test_foreign_region_and_generation(void) {
    ores_task_bound_state_region a =
        ORES_TASK_BOUND_STATE_REGION_INIT(100u, 1u);
    ores_task_bound_state_region b =
        ORES_TASK_BOUND_STATE_REGION_INIT(101u, 1u);
    const ores_state_task_identity owner = task(7u, 21u, 2u, 9u);

    ores_std_sync_result ticket = request_retry(&a, owner, ORES_STD_READ);
    CHECK(ores_state_region_start(&b, owner, ticket.ticket) ==
          ORES_STD_SYNC_STALE);
    CHECK(ores_state_region_release(&b, owner, ticket.ticket).code ==
          ORES_STD_SYNC_STALE);
    start_retry(&a, owner, ticket.ticket);
    end_retry(&a, owner, ticket.ticket);
    CHECK(release_retry(&a, owner, ticket.ticket).code ==
          ORES_STD_SYNC_GRANTED);

    /* Same storage cannot silently accept a ticket from another incarnation. */
    a.generation = 2u;
    a.gate.scope_generation = 2u;
    CHECK(ores_state_region_start(&a, owner, ticket.ticket) ==
          ORES_STD_SYNC_STALE);
}

static void test_reader_cohort_writer_fairness_and_owner_binding(void) {
    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_INIT(200u, 3u);
    const ores_state_task_identity r1 = task(1u, 31u, 1u, 1u);
    const ores_state_task_identity r2 = task(1u, 32u, 2u, 1u);
    const ores_state_task_identity writer = task(1u, 33u, 3u, 1u);
    const ores_state_task_identity late_reader = task(1u, 34u, 4u, 1u);

    ores_std_sync_result a = request_retry(&region, r1, ORES_STD_READ);
    ores_std_sync_result b = request_retry(&region, r2, ORES_STD_READ);
    ores_std_sync_result w = request_retry(&region, writer, ORES_STD_WRITE);
    ores_std_sync_result c = request_retry(&region, late_reader, ORES_STD_READ);
    CHECK(a.code == ORES_STD_SYNC_GRANTED && b.code == ORES_STD_SYNC_GRANTED);
    CHECK(w.code == ORES_STD_SYNC_QUEUED);
    CHECK(c.code == ORES_STD_SYNC_QUEUED);

    start_retry(&region, r1, a.ticket);
    start_retry(&region, r2, b.ticket);
    CHECK(ores_state_region_end(&region, writer, a.ticket) ==
          ORES_STD_SYNC_INVALID);
    end_retry(&region, r1, a.ticket);
    CHECK(release_retry(&region, r1, a.ticket).admitted_count == 0);
    end_retry(&region, r2, b.ticket);
    ores_std_sync_result wake = release_retry(&region, r2, b.ticket);
    CHECK(wake.admitted_count == 1);
    CHECK(wake.admitted[0].sequence == w.ticket.sequence);

    start_retry(&region, writer, w.ticket);
    CHECK(ores_state_region_start(&region, late_reader, c.ticket) ==
          ORES_STD_SYNC_QUEUED);
    end_retry(&region, writer, w.ticket);
    wake = release_retry(&region, writer, w.ticket);
    CHECK(wake.admitted_count == 1);
    CHECK(wake.admitted[0].sequence == c.ticket.sequence);
    start_retry(&region, late_reader, c.ticket);
    end_retry(&region, late_reader, c.ticket);
    CHECK(release_retry(&region, late_reader, c.ticket).code ==
          ORES_STD_SYNC_GRANTED);
}

static void test_seal_revokes_dormant_and_waiting_but_not_running(void) {
    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_INIT(300u, 4u);
    const ores_state_task_identity active = task(1u, 41u, 1u, 1u);
    const ores_state_task_identity queued = task(1u, 42u, 2u, 1u);

    ores_std_sync_result a = request_retry(&region, active, ORES_STD_WRITE);
    ores_std_sync_result q = request_retry(&region, queued, ORES_STD_WRITE);
    start_retry(&region, active, a.ticket);

    ores_std_sync_result stop;
    do { stop = ores_state_region_seal(&region); }
    while (stop.code == ORES_STD_SYNC_RETRY);
    CHECK(stop.code == ORES_STD_SYNC_GRANTED);
    CHECK(stop.failed_count == 1);
    CHECK(stop.failed[0].sequence == q.ticket.sequence);
    CHECK(ores_state_region_start(&region, queued, q.ticket) ==
          ORES_STD_SYNC_STALE);
    CHECK(ores_state_region_quiescent(&region) ==
          ORES_STD_SYNC_EXECUTING);

    end_retry(&region, active, a.ticket);
    CHECK(release_retry(&region, active, a.ticket).code ==
          ORES_STD_SYNC_GRANTED);
    CHECK(ores_state_region_quiescent(&region) ==
          ORES_STD_SYNC_GRANTED);
}

static ores_task_bound_state_region stress_region =
    ORES_TASK_BOUND_STATE_REGION_INIT(500u, 8u);
static uint64_t stress_value = 0;

typedef struct stress_arg {
    ores_state_task_identity owner;
    bool writer;
} stress_arg;

static void* stress_worker(void* payload) {
    stress_arg* arg = (stress_arg*)payload;
    for (unsigned int n = 0; n < 2000u; ++n) {
        const ores_std_access access =
            arg->writer ? ORES_STD_WRITE : ORES_STD_READ;
        ores_std_sync_result ticket =
            request_retry(&stress_region, arg->owner, access);
        start_retry(&stress_region, arg->owner, ticket.ticket);
        if (arg->writer) {
            ++stress_value;
        } else {
            volatile uint64_t snapshot = stress_value;
            (void)snapshot;
        }
        end_retry(&stress_region, arg->owner, ticket.ticket);
        CHECK(release_retry(
            &stress_region, arg->owner, ticket.ticket).code ==
            ORES_STD_SYNC_GRANTED);
    }
    return NULL;
}

static void test_concurrent_task_bound_stress(void) {
    pthread_t threads[8];
    stress_arg args[8];
    for (unsigned int i = 0; i < 8u; ++i) {
        args[i].owner = task(9u, 100u + i, 10u + i, 1u);
        args[i].writer = i < 3u;
        CHECK(pthread_create(&threads[i], NULL, stress_worker, &args[i]) == 0);
    }
    for (unsigned int i = 0; i < 8u; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(stress_value == 6000u);
}

int main(void) {
    test_execution_store_authentication();
    test_identity_and_stale_release();
    test_foreign_region_and_generation();
    test_reader_cohort_writer_fairness_and_owner_binding();
    test_seal_revokes_dormant_and_waiting_but_not_running();
    test_concurrent_task_bound_stress();
    puts("Oreslang task-bound state-region admission PASS");
    return 0;
}
