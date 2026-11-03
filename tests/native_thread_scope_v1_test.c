#include "oreslang/runtime/NativeThreadV1.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int gate = ATOMIC_VAR_INIT(0);
static atomic_int drops = ATOMIC_VAR_INIT(0);
static void drop_arg(void *p) {
    assert(p);
    atomic_fetch_add_explicit(&drops, 1, memory_order_relaxed);
    free(p);
}
static int worker(void *p) {
    const int value = *(int *)p;
    while (!atomic_load_explicit(&gate, memory_order_acquire))
        sched_yield();
    return value;
}
static int *owned(int value) {
    int *p = malloc(sizeof(*p));
    assert(p);
    *p = value;
    return p;
}
static void joined_release(ores_native_thread_v1 *h, int expected) {
    int result = -1;
    assert(ores_native_thread_join_blocking_v1(h, &result) == ORES_THREAD_OK_V1);
    assert(result == expected);
    assert(ores_native_thread_release_v1(h) == ORES_THREAD_OK_V1);
}
static void basic_close_and_lifetime(void) {
    ores_native_thread_scope_v1 *scope = NULL, *other = NULL;
    assert(ores_native_thread_scope_open_v1(0, &scope) == ORES_THREAD_BAD_ARGUMENT_V1);
    assert(scope == NULL);
    assert(ores_native_thread_scope_open_v1(1, &scope) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_SCOPE_BUSY_V1);
    assert(ores_native_thread_scope_open_v1(1, &other) == ORES_THREAD_OK_V1);
    ores_native_thread_v1 *h = NULL, *independent = NULL;
    assert(ores_native_thread_scope_spawn_v1(scope, worker, owned(11), drop_arg,
                                               &h) == ORES_THREAD_OK_V1);
    unsigned outstanding = 0;
    assert(ores_native_thread_scope_outstanding_v1(scope, &outstanding)
           == ORES_THREAD_OK_V1 && outstanding == 1);
    int *retained = owned(12);
    ores_native_thread_v1 *denied = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_scope_spawn_v1(scope, worker, retained, drop_arg,
                                               &denied) == ORES_THREAD_SCOPE_FULL_V1);
    assert(denied == NULL);
    assert(*retained == 12);
    free(retained); /* rejected owned argument was not consumed */
    /* An independent isolate cannot borrow this scope's capacity, but it
     * can use its own capacity while the first scope is saturated. */
    assert(ores_native_thread_scope_spawn_v1(other, worker, owned(13), drop_arg,
                                               &independent) == ORES_THREAD_OK_V1);
    atomic_store_explicit(&gate, 1, memory_order_release);
    int result = -1;
    assert(ores_native_thread_join_blocking_v1(h, &result) == ORES_THREAD_OK_V1);
    assert(result == 11);
    /* Physical thread termination and join do not release scope quota
     * until the unique handle has also been safely released. */
    retained = owned(14);
    assert(ores_native_thread_scope_spawn_v1(scope, worker, retained, drop_arg,
                                               &denied) == ORES_THREAD_SCOPE_FULL_V1);
    free(retained);
    assert(ores_native_thread_scope_close_v1(scope) == ORES_THREAD_SCOPE_BUSY_V1);
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_SCOPE_BUSY_V1);
    assert(ores_native_thread_release_v1(h) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_close_v1(scope) == ORES_THREAD_OK_V1);
    retained = owned(15);
    denied = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_scope_spawn_v1(scope, worker, retained, drop_arg,
                                               &denied) == ORES_THREAD_SCOPE_CLOSED_V1);
    assert(denied == NULL);
    free(retained);
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_OK_V1);
    joined_release(independent, 13);
    assert(ores_native_thread_scope_close_v1(other) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(other) == ORES_THREAD_OK_V1);
}
#define CALLERS 12
#define CAPACITY 3
static ores_native_thread_scope_v1 *shared_scope;
static ores_native_thread_v1 *accepted[CALLERS];
static atomic_int accepted_count = ATOMIC_VAR_INIT(0);
static atomic_int rejected_count = ATOMIC_VAR_INIT(0);
static void *contender(void *context) {
    const int id = *(int *)context;
    int *arg = owned(100 + id);
    ores_native_thread_v1 *handle = (ores_native_thread_v1 *)1;
    const ores_thread_status_v1 rc = ores_native_thread_scope_spawn_v1(
        shared_scope, worker, arg, drop_arg, &handle);
    if (rc == ORES_THREAD_OK_V1) {
        assert(handle);
        accepted[id] = handle;
        atomic_fetch_add(&accepted_count, 1);
    } else {
        assert(rc == ORES_THREAD_SCOPE_FULL_V1);
        assert(handle == NULL);
        free(arg);
        atomic_fetch_add(&rejected_count, 1);
    }
    return NULL;
}
static void concurrent_admission(void) {
    assert(ores_native_thread_scope_open_v1(CAPACITY, &shared_scope)
           == ORES_THREAD_OK_V1);
    atomic_store(&gate, 0);
    pthread_t callers[CALLERS];
    int ids[CALLERS];
    for (int i = 0; i < CALLERS; ++i) {
        ids[i] = i;
        assert(pthread_create(&callers[i], NULL, contender, &ids[i]) == 0);
    }
    for (int i = 0; i < CALLERS; ++i)
        assert(pthread_join(callers[i], NULL) == 0);
    assert(atomic_load(&accepted_count) == CAPACITY);
    assert(atomic_load(&rejected_count) == CALLERS - CAPACITY);
    unsigned count = 0;
    assert(ores_native_thread_scope_outstanding_v1(shared_scope, &count)
           == ORES_THREAD_OK_V1 && count == CAPACITY);
    assert(ores_native_thread_scope_close_v1(shared_scope)
           == ORES_THREAD_SCOPE_BUSY_V1);
    atomic_store_explicit(&gate, 1, memory_order_release);
    for (int i = 0; i < CALLERS; ++i) {
        if (accepted[i]) joined_release(accepted[i], 100 + i);
    }
    assert(ores_native_thread_scope_outstanding_v1(shared_scope, &count)
           == ORES_THREAD_OK_V1 && count == 0);
    assert(ores_native_thread_scope_close_v1(shared_scope) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(shared_scope) == ORES_THREAD_OK_V1);
}
#ifdef ORES_NATIVE_THREAD_TEST_ID_EXHAUSTION
static void rollback_on_failed_native_spawn(void) {
    ores_native_thread_scope_v1 *scope = NULL;
    assert(ores_native_thread_scope_open_v1(1, &scope) == ORES_THREAD_OK_V1);
    ores_native_thread_test_set_next_id_v1(UINT64_MAX);
    int *arg = owned(222);
    ores_native_thread_v1 *h = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_scope_spawn_v1(scope, worker, arg, drop_arg, &h)
           == ORES_THREAD_OS_ERROR_V1);
    assert(h == NULL);
    assert(*arg == 222); /* failed physical spawn did not transfer ownership */
    free(arg);
    unsigned count = 999;
    assert(ores_native_thread_scope_outstanding_v1(scope, &count)
           == ORES_THREAD_OK_V1 && count == 0);
    assert(ores_native_thread_scope_close_v1(scope) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_OK_V1);
}
#endif
static int premature_exit(void *p) {
    (void)p;
    pthread_exit(NULL);
    return 0; /* unreachable */
}
static void abnormal_exit_retains_quota_until_release(void) {
    ores_native_thread_scope_v1 *scope = NULL;
    assert(ores_native_thread_scope_open_v1(1, &scope) == ORES_THREAD_OK_V1);
    ores_native_thread_v1 *h = NULL;
    assert(ores_native_thread_scope_spawn_v1(
        scope, premature_exit, owned(88), drop_arg, &h) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_close_v1(scope) == ORES_THREAD_SCOPE_BUSY_V1);
    int result = -999;
    assert(ores_native_thread_join_blocking_v1(h, &result)
           == ORES_THREAD_UNEXPECTED_EXIT_V1);
    assert(result == -999); /* no fabricated successful guest return */
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_SCOPE_BUSY_V1);
    assert(ores_native_thread_release_v1(h) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_close_v1(scope) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(scope) == ORES_THREAD_OK_V1);
}
static void isolate_identity_fences_v1(void) {
    const ores_native_thread_isolate_key_v1 expected = {91, 13, 4};
    const ores_native_thread_isolate_key_v1 wrong_tenant = {92, 13, 4};
    const ores_native_thread_isolate_key_v1 wrong_isolate = {91, 14, 4};
    const ores_native_thread_isolate_key_v1 stale_generation = {91, 13, 3};
    const ores_native_thread_isolate_key_v1 invalid = {0, 13, 4};
    ores_native_thread_scope_v1 *scope = (ores_native_thread_scope_v1 *)1;
    assert(ores_native_thread_scope_open_isolate_v1(invalid, 1, &scope)
           == ORES_THREAD_BAD_ARGUMENT_V1);
    assert(scope == NULL);
    assert(ores_native_thread_scope_open_isolate_v1(expected, 1, &scope)
           == ORES_THREAD_OK_V1);

    const ores_native_thread_isolate_key_v1 wrong[] = {
        wrong_tenant, wrong_isolate, stale_generation
    };
    for (unsigned i = 0; i < sizeof(wrong)/sizeof(wrong[0]); ++i) {
        int *arg = owned(120 + (int)i);
        ores_native_thread_v1 *handle = (ores_native_thread_v1 *)1;
        assert(ores_native_thread_scope_spawn_isolate_v1(
                   scope, wrong[i], worker, arg, drop_arg, &handle)
               == ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1);
        assert(handle == NULL);
        assert(*arg == 120 + (int)i);
        free(arg); /* rejected cross-tenant/epoch source keeps ownership */
        assert(ores_native_thread_scope_close_isolate_v1(scope, wrong[i])
               == ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1);
        assert(ores_native_thread_scope_destroy_isolate_v1(scope, wrong[i])
               == ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1);
    }
    unsigned outstanding = 99;
    assert(ores_native_thread_scope_outstanding_v1(scope, &outstanding)
           == ORES_THREAD_OK_V1 && outstanding == 0);

    ores_native_thread_v1 *handle = NULL;
    assert(ores_native_thread_scope_spawn_isolate_v1(
               scope, expected, worker, owned(123), drop_arg, &handle)
           == ORES_THREAD_OK_V1);
    assert(handle != NULL);
    assert(ores_native_thread_scope_close_isolate_v1(scope, stale_generation)
           == ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1);
    assert(ores_native_thread_scope_close_isolate_v1(scope, expected)
           == ORES_THREAD_SCOPE_BUSY_V1);
    assert(ores_native_thread_scope_destroy_isolate_v1(scope, expected)
           == ORES_THREAD_SCOPE_BUSY_V1);
    joined_release(handle, 123);
    assert(ores_native_thread_scope_close_isolate_v1(scope, expected)
           == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_isolate_v1(scope, expected)
           == ORES_THREAD_OK_V1);

    /* A legacy unbound host-only scope must never be mistaken for a keyed
     * isolate-scoped capability even when the request fields look valid. */
    ores_native_thread_scope_v1 *legacy = NULL;
    assert(ores_native_thread_scope_open_v1(1, &legacy) == ORES_THREAD_OK_V1);
    int *legacy_arg = owned(124);
    ores_native_thread_v1 *denied = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_scope_spawn_isolate_v1(
               legacy, expected, worker, legacy_arg, drop_arg, &denied)
           == ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1);
    assert(denied == NULL && *legacy_arg == 124);
    free(legacy_arg);
    assert(ores_native_thread_scope_close_v1(legacy) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_scope_destroy_v1(legacy) == ORES_THREAD_OK_V1);
}

int main(void) {
    ores_native_thread_scope_v1 *bad = (ores_native_thread_scope_v1 *)1;
    assert(ores_native_thread_scope_open_v1(1025, &bad)
           == ORES_THREAD_BAD_ARGUMENT_V1);
    assert(bad == NULL);
    basic_close_and_lifetime();
    concurrent_admission();
    abnormal_exit_retains_quota_until_release();
    isolate_identity_fences_v1();
#ifdef ORES_NATIVE_THREAD_TEST_ID_EXHAUSTION
    rollback_on_failed_native_spawn();
#endif
    assert(atomic_load(&drops) == 7);
    puts("NATIVE_THREAD_SCOPE_QUOTA_PASS (host-only)");
    return 0;
}
