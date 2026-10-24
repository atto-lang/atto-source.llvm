#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeContinuationStore.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void verify(int condition, const char* what, int line) {
    if (!condition) {
        fprintf(stderr, "CONTINUATION_READY_FAIL line=%d: %s\n", line, what);
        abort();
    }
}
#define check(expr) verify(!!(expr), #expr, __LINE__)

typedef struct {
    OresNativeContinuationStore* store;
    OresNativeContinuationHandle handle;
    atomic_int* go;
    int is_cancel;
    int requires_ready;
    int won;
} RaceArgs;

static void* contender(void* raw) {
    RaceArgs* race = (RaceArgs*)raw;
    while (!atomic_load_explicit(race->go, memory_order_acquire)) {
        /* Spin only in this bounded host test; no production carrier parks. */
    }
    if (race->is_cancel) {
        race->won = ores_native_continuation_store_cancel(
            race->store, race->handle);
    } else if (race->requires_ready) {
        race->won = ores_native_continuation_store_try_claim_ready_resume(
            race->store, race->handle, NULL);
    } else {
        race->won = ores_native_continuation_store_notify_ready(
            race->store, race->handle);
    }
    return NULL;
}

static void race_pair(RaceArgs* first, RaceArgs* second) {
    atomic_int go = ATOMIC_VAR_INIT(0);
    first->go = &go;
    second->go = &go;
    pthread_t one, two;
    check(pthread_create(&one, NULL, contender, first) == 0);
    check(pthread_create(&two, NULL, contender, second) == 0);
    atomic_store_explicit(&go, 1, memory_order_release);
    check(pthread_join(one, NULL) == 0);
    check(pthread_join(two, NULL) == 0);
    check(first->won + second->won == 1);
}

static OresNativeContinuationHandle publish(
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle execution) {
    OresNativeContinuationHandle handle = {0, 0};
    check(ores_native_continuation_store_publish(
        continuations, execution, (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 100, &handle));
    check(handle.slot != 0 && handle.generation != 0);
    return handle;
}

int main(void) {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(4);
    check(executions != NULL);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(2, executions);
    check(continuations != NULL);
    OresNativeExecutionContext context = {
        .id = {88, 7},
        .allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4},
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionHandle execution = {0, 0};
    check(ores_native_execution_context_store_insert(
        executions, &context, &execution));

    /* A not-yet-ready Future must not claim the source continuation. */
    OresNativeContinuationHandle pending = publish(continuations, execution);
    OresNativeContinuationRecord record = {0};
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, &record));
    check(ores_native_continuation_store_notify_ready(continuations, pending));
    check(!ores_native_continuation_store_notify_ready(continuations, pending));
    check(ores_native_continuation_store_get(continuations, pending, &record));
    check(record.state == ORES_CONTINUATION_READY);
    check(ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    check(!ores_native_continuation_store_cancel(continuations, pending));
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, NULL));
    check(ores_native_continuation_store_finish_resume(continuations, pending));
    check(!ores_native_continuation_store_finish_resume(continuations, pending));
    check(ores_native_continuation_store_release(continuations, pending));

    /* Cancellation before Future readiness is irrevocable. */
    OresNativeContinuationHandle cancelled = publish(continuations, execution);
    check(ores_native_continuation_store_cancel(continuations, cancelled));
    check(!ores_native_continuation_store_notify_ready(continuations, cancelled));
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, cancelled, NULL));
    check(ores_native_continuation_store_release(continuations, cancelled));
    check(!ores_native_continuation_store_get(continuations, cancelled, &record));

    /*
     * Stress two distinct linearization windows:
     * (1) completion-vs-cancel while PENDING;
     * (2) cancellation-vs-resume claim after READY.
     * Neither losing side can cause a second terminal execution.
     */
    for (int iteration = 0; iteration < 512; ++iteration) {
        OresNativeContinuationHandle handle = publish(continuations, execution);
        RaceArgs cancel = {continuations, handle, NULL, 1, 0, 0};
        RaceArgs complete = {continuations, handle, NULL, 0, 0, 0};
        race_pair(&cancel, &complete);
        check(ores_native_continuation_store_get(continuations, handle, &record));
        if (cancel.won) {
            check(record.state == ORES_CONTINUATION_CANCELLED);
            check(!ores_native_continuation_store_try_claim_ready_resume(
                continuations, handle, NULL));
        } else {
            check(record.state == ORES_CONTINUATION_READY);
            check(ores_native_continuation_store_try_claim_ready_resume(
                continuations, handle, NULL));
            check(ores_native_continuation_store_finish_resume(
                continuations, handle));
        }
        check(ores_native_continuation_store_release(continuations, handle));
        check(!ores_native_continuation_store_notify_ready(
            continuations, handle));

        OresNativeContinuationHandle ready = publish(continuations, execution);
        check(ores_native_continuation_store_notify_ready(continuations, ready));
        RaceArgs canceller = {continuations, ready, NULL, 1, 0, 0};
        RaceArgs claimer = {continuations, ready, NULL, 0, 1, 0};
        race_pair(&canceller, &claimer);
        check(ores_native_continuation_store_get(continuations, ready, &record));
        if (claimer.won) {
            check(record.state == ORES_CONTINUATION_CLAIMED);
            check(ores_native_continuation_store_finish_resume(
                continuations, ready));
        } else {
            check(record.state == ORES_CONTINUATION_CANCELLED);
            check(!ores_native_continuation_store_finish_resume(
                continuations, ready));
        }
        check(!ores_native_continuation_store_notify_ready(continuations, ready));
        check(!ores_native_continuation_store_try_claim_ready_resume(
            continuations, ready, NULL));
        check(ores_native_continuation_store_release(continuations, ready));
        check(!ores_native_continuation_store_get(continuations, ready, &record));
    }

    check(ores_native_continuation_store_live(continuations) == 0);
    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_release(executions, execution));
    check(ores_native_execution_context_store_close(executions));
    puts("NATIVE_CONTINUATION_READY_CANCEL_RESUME_PASS");
    return 0;
}
