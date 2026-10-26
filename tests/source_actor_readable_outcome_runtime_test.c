#include "oreslang/runtime/SourceActorBootstrap.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check(int ok) { if (!ok) abort(); }

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
}

static int64_t actor_message(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    return payload;
}

static void check_actor_lane_trace(uint64_t token) {
    OresNativeExecutionHandle current = {0, 0};
    check(oreslang_source_actor_current_execution_handle(&current));
    check(ores_native_store_handle_valid(current));

    OresNativeExecutionTraceFrame executions[4] = {0};
    OresNativeContinuationTraceFrame continuations[4] = {0};
    OresNativeLogicalTraceResult trace = {0};
    check(oreslang_source_actor_snapshot_current_trace(
        executions, 4, continuations, 4, &trace));
    check(!trace.corrupt && !trace.execution_truncated
        && !trace.continuation_truncated);
    check(trace.execution_count == 2);
    check(trace.continuation_count == 1);
    check(continuations[0].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuations[0].record.source_token == token);
    check(continuations[0].record.state == ORES_CONTINUATION_CLAIMED);
    check(continuations[0].record.code_generation == 1);
}

static int64_t resumed_cancelled(
        void* actor, OresSourceReadableOutcome outcome) {
    (void)actor;
    check(outcome == ORES_SOURCE_READABLE_CANCELLED);
    check_actor_lane_trace(UINT64_C(0x6161));
    return 44;
}

static int64_t resumed_failed(
        void* actor, OresSourceReadableOutcome outcome) {
    (void)actor;
    check(outcome == ORES_SOURCE_READABLE_FAILED);
    check_actor_lane_trace(UINT64_C(0x7171));
    return 45;
}

static void* cancel_main(void* ignored) {
    (void)ignored;
    for (int retry = 0; retry < 100000; ++retry) {
        if (oreslang_source_actor_test_cancel_pending_readable())
            return NULL;
        sched_yield();
    }
    abort();
}

static void run_cancelled(void) {
    int pipefd[2] = {-1, -1};
    check(pipe(pipefd) == 0);
    const int flags = fcntl(pipefd[0], F_GETFL);
    check(flags >= 0);
    check(fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == 0);

    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    pthread_t canceller;
    check(pthread_create(&canceller, NULL, cancel_main, NULL) == 0);
    const int64_t result = oreslang_source_actor_await_readable(
        actor, pipefd[0], resumed_cancelled, UINT64_C(0x6161));
    check(result == 44);
    check(pthread_join(canceller, NULL) == 0);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    close(pipefd[0]);
    close(pipefd[1]);
    oreslang_source_actor_stop(actor);
}

static void run_failed_admission(void) {
    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    /*
     * INT32_MAX is admitted by the source ABI but cannot be a live process FD
     * here. Unlike a recently closed low fd, lazy reactor startup cannot reuse
     * it and accidentally turn the failure test into a valid watch.
     */
    const int64_t result = oreslang_source_actor_await_readable(
        actor, INT32_MAX, resumed_failed, UINT64_C(0x7171));
    check(result == 45);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    oreslang_source_actor_stop(actor);
}

int main(void) {
    run_cancelled();
    run_failed_admission();
    puts("SOURCE_ACTOR_READABLE_OUTCOME_RUNTIME_PASS");
    return 0;
}
