#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeAwaitTraceBridge.h"

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

typedef struct {
    int calls;
    int result;
} ReadyCommitProbe;

static int ready_commit_probe(void* raw) {
    ReadyCommitProbe* probe = (ReadyCommitProbe*)raw;
    ++probe->calls;
    return probe->result;
}


typedef struct {
    OresNativeContinuationStore* store;
    OresNativeContinuationHandle handle;
    int calls;
} ReentryProbe;

static int reentry_probe(void* raw) {
    ReentryProbe* probe = (ReentryProbe*)raw;
    OresNativeContinuationRecord ignored = {0};
    ++probe->calls;
    /* Must fail immediately rather than deadlock on the held store lock. */
    return !ores_native_continuation_store_get(
        probe->store, probe->handle, &ignored);
}


typedef struct {
    atomic_int entered;
    atomic_int release;
} BlockingCommitProbe;

typedef struct {
    OresNativeContinuationStore* store;
    OresNativeContinuationHandle handle;
    BlockingCommitProbe* probe;
    int won;
} CommitRaceArgs;

typedef struct {
    OresNativeContinuationStore* store;
    OresNativeContinuationHandle handle;
    int won;
} CancelRaceArgs;

static int blocking_ready_commit(void* raw) {
    BlockingCommitProbe* probe = (BlockingCommitProbe*)raw;
    atomic_store_explicit(&probe->entered, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) {
        /* Hold the producer commit open while cancellation contends. */
    }
    return 1;
}

static void* commit_ready_contender(void* raw) {
    CommitRaceArgs* race = (CommitRaceArgs*)raw;
    race->won = ores_native_continuation_store_commit_ready(
        race->store, race->handle, blocking_ready_commit, race->probe);
    return NULL;
}

static void* cancel_ready_contender(void* raw) {
    CancelRaceArgs* race = (CancelRaceArgs*)raw;
    race->won = ores_native_continuation_store_cancel(
        race->store, race->handle);
    return NULL;
}

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

    /*
     * The producer commit and READY publication form one transaction relative
     * to cancellation. A failed producer commit leaves PENDING unchanged.
     */
    OresNativeContinuationRecord record = {0};
    ReadyCommitProbe probe = {0, 1};
    OresNativeContinuationHandle transactional_ready =
        publish(continuations, execution);
    check(ores_native_continuation_store_commit_ready(
        continuations, transactional_ready, ready_commit_probe, &probe));
    check(probe.calls == 1);
    check(ores_native_continuation_store_get(
        continuations, transactional_ready, &record));
    check(record.state == ORES_CONTINUATION_READY);
    check(ores_native_continuation_store_cancel(
        continuations, transactional_ready));
    check(ores_native_continuation_store_release(
        continuations, transactional_ready));

    probe = (ReadyCommitProbe){0, 0};
    OresNativeContinuationHandle failed_commit =
        publish(continuations, execution);
    check(!ores_native_continuation_store_commit_ready(
        continuations, failed_commit, ready_commit_probe, &probe));
    check(probe.calls == 1);
    check(ores_native_continuation_store_get(
        continuations, failed_commit, &record));
    check(record.state == ORES_CONTINUATION_PENDING);
    check(ores_native_continuation_store_cancel(continuations, failed_commit));
    check(ores_native_continuation_store_release(continuations, failed_commit));


    OresNativeContinuationHandle reentry =
        publish(continuations, execution);
    ReentryProbe reentry_probe_state = {continuations, reentry, 0};
    check(ores_native_continuation_store_commit_ready(
        continuations, reentry, reentry_probe, &reentry_probe_state));
    check(reentry_probe_state.calls == 1);
    check(ores_native_continuation_store_get(continuations, reentry, &record));
    check(record.state == ORES_CONTINUATION_READY);
    check(ores_native_continuation_store_cancel(continuations, reentry));
    check(ores_native_continuation_store_release(continuations, reentry));


    /*
     * Cross-store re-entry is also rejected: otherwise concurrent callbacks
     * could acquire A->B and B->A and deadlock even though neither re-enters
     * its own store.
     */
    OresNativeExecutionContextStore* other_executions =
        ores_native_execution_context_store_open(2);
    check(other_executions != NULL);
    OresNativeExecutionContext other_context = context;
    other_context.id.runtime_domain = 99;
    other_context.id.execution = 1;
    other_context.allocation.domain = 99;
    other_context.allocation.owner = 4;
    OresNativeExecutionHandle other_execution = {0, 0};
    check(ores_native_execution_context_store_insert(
        other_executions, &other_context, &other_execution));
    OresNativeContinuationStore* other_continuations =
        ores_native_continuation_store_open(1, other_executions);
    check(other_continuations != NULL);
    OresNativeContinuationHandle other_handle =
        publish(other_continuations, other_execution);

    OresNativeContinuationHandle cross_store_reentry =
        publish(continuations, execution);
    ReentryProbe cross_store_probe = {
        other_continuations, other_handle, 0
    };
    check(ores_native_continuation_store_commit_ready(
        continuations, cross_store_reentry,
        reentry_probe, &cross_store_probe));
    check(cross_store_probe.calls == 1);
    check(ores_native_continuation_store_get(
        continuations, cross_store_reentry, &record));
    check(record.state == ORES_CONTINUATION_READY);
    check(ores_native_continuation_store_get(
        other_continuations, other_handle, &record));
    check(record.state == ORES_CONTINUATION_PENDING);

    check(ores_native_continuation_store_cancel(
        continuations, cross_store_reentry));
    check(ores_native_continuation_store_release(
        continuations, cross_store_reentry));
    check(ores_native_continuation_store_cancel(
        other_continuations, other_handle));
    check(ores_native_continuation_store_release(
        other_continuations, other_handle));
    check(ores_native_continuation_store_close(other_continuations));
    check(ores_native_execution_context_store_release(
        other_executions, other_execution));
    check(ores_native_execution_context_store_close(other_executions));


    /*
     * Deterministic commit-vs-cancel contention. The producer callback blocks
     * while commit_ready owns the continuation-store lock. Cancellation may
     * only proceed after READY is published, so both calls may succeed but
     * the final state must be CANCELLED rather than READY being written over
     * an intervening cancellation.
     */
    OresNativeContinuationHandle serialized_commit =
        publish(continuations, execution);
    BlockingCommitProbe blocking_probe = {
        .entered = ATOMIC_VAR_INIT(0),
        .release = ATOMIC_VAR_INIT(0)
    };
    CommitRaceArgs commit_race = {
        continuations, serialized_commit, &blocking_probe, 0
    };
    pthread_t commit_thread;
    check(pthread_create(
        &commit_thread, NULL, commit_ready_contender, &commit_race) == 0);
    while (!atomic_load_explicit(
            &blocking_probe.entered, memory_order_acquire)) {
        /* Wait until the fallible producer commit runs under the store lock. */
    }
    CancelRaceArgs cancel_race = {continuations, serialized_commit, 0};
    pthread_t cancel_thread;
    check(pthread_create(
        &cancel_thread, NULL, cancel_ready_contender, &cancel_race) == 0);
    atomic_store_explicit(&blocking_probe.release, 1, memory_order_release);
    check(pthread_join(commit_thread, NULL) == 0);
    check(pthread_join(cancel_thread, NULL) == 0);
    check(commit_race.won);
    check(cancel_race.won);
    check(ores_native_continuation_store_get(
        continuations, serialized_commit, &record));
    check(record.state == ORES_CONTINUATION_CANCELLED);
    check(!ores_native_continuation_store_notify_ready(
        continuations, serialized_commit));
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, serialized_commit, NULL));
    check(!ores_native_continuation_store_finish_resume(
        continuations, serialized_commit));
    check(ores_native_continuation_store_release(
        continuations, serialized_commit));

    /* A not-yet-ready Future must not claim the source continuation. */
    OresNativeContinuationHandle pending = publish(continuations, execution);
    record = (OresNativeContinuationRecord){0};
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, &record));
    check(ores_native_continuation_store_notify_ready(continuations, pending));
    check(!ores_native_continuation_store_notify_ready(continuations, pending));
    check(ores_native_continuation_store_get(continuations, pending, &record));
    check(record.state == ORES_CONTINUATION_READY);
    check(ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    /* A physical execution cannot lose its final owning pin while CLAIMED. */
    check(!ores_native_continuation_store_release(continuations, pending));
    check(ores_native_continuation_store_get(continuations, pending, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    check(!ores_native_continuation_store_cancel(continuations, pending));
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, pending, NULL));
    check(ores_native_continuation_store_finish_resume(continuations, pending));
    check(!ores_native_continuation_store_finish_resume(continuations, pending));
    check(ores_native_continuation_store_release(continuations, pending));

    /*
     * A nested child release may reach a CLAIMED parent with only its final
     * execution pin remaining. Child cleanup succeeds, but the parent must
     * stay CLAIMED until its actual physical owner completes and releases.
     */
    OresNativeContinuationHandle claimed_parent =
        publish(continuations, execution);
    check(ores_native_continuation_store_notify_ready(
        continuations, claimed_parent));
    check(ores_native_continuation_store_try_claim_ready_resume(
        continuations, claimed_parent, &record));
    OresNativeContinuationHandle nested = {0, 0};
    check(ores_native_continuation_store_publish(
        continuations, execution, claimed_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 101, &nested));
    check(ores_native_continuation_store_release(
        continuations, claimed_parent)); /* drop original parent owner pin */
    check(ores_native_continuation_store_release(
        continuations, nested)); /* cascade reaches protected parent */
    check(!ores_native_continuation_store_get(
        continuations, nested, &record));
    check(ores_native_continuation_store_get(
        continuations, claimed_parent, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    check(!ores_native_continuation_store_release(
        continuations, claimed_parent));
    check(ores_native_continuation_store_finish_resume(
        continuations, claimed_parent));
    check(ores_native_continuation_store_release(
        continuations, claimed_parent));
    check(!ores_native_continuation_store_get(
        continuations, claimed_parent, &record));

    /* Cancellation before Future readiness is irrevocable. */
    OresNativeContinuationHandle cancelled = publish(continuations, execution);
    check(ores_native_continuation_store_cancel(continuations, cancelled));
    ReadyCommitProbe cancelled_probe = {0, 1};
    check(!ores_native_continuation_store_commit_ready(
        continuations, cancelled, ready_commit_probe, &cancelled_probe));
    check(cancelled_probe.calls == 0);
    check(!ores_native_continuation_store_notify_ready(continuations, cancelled));
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, cancelled, NULL));
    check(ores_native_continuation_store_release(continuations, cancelled));
    check(!ores_native_continuation_store_get(continuations, cancelled, &record));

    /*
     * READY is still cancellable until a resume claim linearizes. Both
     * notify_ready() and cancel() may therefore report success in sequence;
     * the observable terminal state must be CANCELLED and unclaimable.
     */
    OresNativeContinuationHandle ready_then_cancel =
        publish(continuations, execution);
    check(ores_native_continuation_store_notify_ready(
        continuations, ready_then_cancel));
    check(ores_native_continuation_store_cancel(
        continuations, ready_then_cancel));
    check(ores_native_continuation_store_get(
        continuations, ready_then_cancel, &record));
    check(record.state == ORES_CONTINUATION_CANCELLED);
    check(!ores_native_continuation_store_try_claim_ready_resume(
        continuations, ready_then_cancel, NULL));
    check(ores_native_continuation_store_release(
        continuations, ready_then_cancel));


    /*
     * ABA regression: a released handle must never affect a reused slot.
     * Capacity two ensures repeated publication eventually reuses it.
     */
    OresNativeContinuationHandle stale = cancelled;
    int observed_reused_slot = 0;
    for (int reuse = 0; reuse < 8; ++reuse) {
        OresNativeContinuationHandle current = publish(continuations, execution);
        check(!ores_native_continuation_store_notify_ready(continuations, stale));
        check(!ores_native_continuation_store_cancel(continuations, stale));
        check(!ores_native_continuation_store_try_claim_ready_resume(
            continuations, stale, NULL));
        check(!ores_native_continuation_store_finish_resume(
            continuations, stale));
        check(!ores_native_continuation_store_get(continuations, stale, &record));
        check(ores_native_continuation_store_get(continuations, current, &record));
        check(record.state == ORES_CONTINUATION_PENDING);
        if (current.slot == stale.slot) {
            observed_reused_slot = 1;
            check(current.generation != stale.generation);
            check(!ores_native_continuation_store_release(continuations, stale));
        }
        check(ores_native_continuation_store_cancel(continuations, current));
        check(ores_native_continuation_store_release(continuations, current));
    }
    check(observed_reused_slot);

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
        check(cancel.won || complete.won);
        check(ores_native_continuation_store_get(continuations, handle, &record));
        if (cancel.won) {
            /*
             * notify_ready may linearize first and still report success;
             * cancellation is allowed to win afterward while READY remains
             * unclaimed. What matters is the terminal state, not that only
             * one transition API returned success.
             */
            check(record.state == ORES_CONTINUATION_CANCELLED);
            check(!ores_native_continuation_store_try_claim_ready_resume(
                continuations, handle, NULL));
        } else {
            check(complete.won);
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
        check(canceller.won + claimer.won == 1);
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

    /*
     * Exercise the exact bridge called from real compiled actor await:
     * a Future cannot re-enter a root actor turn while still PENDING,
     * even if the host tries to schedule it with a forged new execution ID.
     */
    OresNativeExecutionContext trace_origin_context = context;
    trace_origin_context.id.execution = 8;
    OresNativeExecutionHandle trace_origin = {0, 0};
    check(ores_native_execution_context_store_insert(
        executions, &trace_origin_context, &trace_origin));
    OresNativeSuspensionLink link = {0};
    check(ores_native_await_trace_suspend(
        executions, continuations, trace_origin,
        (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 77, 1300, &link));
    check(ores_native_execution_context_store_release(executions, trace_origin));

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext resumed_context = {0};
    check(!ores_native_await_trace_ready_resume_begin(
        executions, continuations, (OresNativeExecutionId){88, 9},
        link, &resumed, &resumed_context));
    check(ores_native_continuation_store_get(
        continuations, link.continuation, &record));
    check(record.state == ORES_CONTINUATION_PENDING);
    check(ores_native_continuation_store_notify_ready(
        continuations, link.continuation));
    check(ores_native_await_trace_ready_resume_begin(
        executions, continuations, (OresNativeExecutionId){88, 9},
        link, &resumed, &resumed_context));
    check(resumed_context.id.execution == 9);
    check(resumed_context.allocation.owner == context.allocation.owner);
    check(ores_native_continuation_store_get(
        continuations, link.continuation, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    check(!ores_native_await_trace_ready_resume_begin(
        executions, continuations, (OresNativeExecutionId){88, 10},
        link, &trace_origin, NULL));
    /* Forged or recycled physical ownership must never finish a guest turn. */
    OresNativeSuspensionLink forged_finish = link;
    ++forged_finish.parent.slot;
    check(!ores_native_await_trace_resume_finish(
        continuations, forged_finish));
    forged_finish = link;
    ++forged_finish.parent.generation;
    check(!ores_native_await_trace_resume_finish(
        continuations, forged_finish));
    forged_finish = link;
    ++forged_finish.continuation.generation;
    check(!ores_native_await_trace_resume_finish(
        continuations, forged_finish));
    forged_finish = link;
    ++forged_finish.execution_id.execution;
    check(!ores_native_await_trace_resume_finish(
        continuations, forged_finish));
    forged_finish = link;
    ++forged_finish.execution_id.runtime_domain;
    check(!ores_native_await_trace_resume_finish(
        continuations, forged_finish));
    check(ores_native_continuation_store_get(
        continuations, link.continuation, &record));
    check(record.state == ORES_CONTINUATION_CLAIMED);
    check(ores_native_await_trace_resume_finish(continuations, link));
    check(!ores_native_await_trace_resume_finish(continuations, link));
    check(ores_native_execution_context_store_release(executions, resumed));
    check(ores_native_continuation_store_release(
        continuations, link.continuation));

    /* A cancellation wins over any late ready event or scheduled callback. */
    trace_origin_context.id.execution = 11;
    check(ores_native_execution_context_store_insert(
        executions, &trace_origin_context, &trace_origin));
    OresNativeSuspensionLink cancelled_link = {0};
    check(ores_native_await_trace_suspend(
        executions, continuations, trace_origin,
        (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 77, 1301, &cancelled_link));
    check(ores_native_execution_context_store_release(executions, trace_origin));
    check(ores_native_continuation_store_cancel(
        continuations, cancelled_link.continuation));
    check(!ores_native_await_trace_ready_resume_begin(
        executions, continuations, (OresNativeExecutionId){88, 12},
        cancelled_link, &resumed, NULL));
    check(!ores_native_continuation_store_notify_ready(
        continuations, cancelled_link.continuation));
    check(ores_native_continuation_store_release(
        continuations, cancelled_link.continuation));

    check(ores_native_continuation_store_live(continuations) == 0);
    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_release(executions, execution));
    check(ores_native_execution_context_store_close(executions));
    puts("NATIVE_CONTINUATION_READY_CANCEL_RESUME_PASS");
    return 0;
}
