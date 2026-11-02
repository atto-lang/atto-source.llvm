#include "oreslang/runtime/NativeThreadV1.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int drops = ATOMIC_VAR_INIT(0);
static atomic_int entry_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t seen_ids[4];

static void drop_value(void *ptr) {
    assert(ptr);
    atomic_fetch_add(&drops, 1);
    free(ptr);
}
static int *new_value(int value) {
    int *p = malloc(sizeof(*p));
    assert(p);
    *p = value;
    return p;
}
typedef struct { int idx; } args;
static int worker(void *opaque) {
    args *a = (args *)opaque;
    uint64_t id = 0;
    assert(ores_native_thread_id_v1(&id) == ORES_THREAD_OK_V1);
    assert(id != 0);
    atomic_store(&seen_ids[a->idx], id);
    void *v = NULL;
    assert(ores_native_thread_local_get_v1(1, &v) == ORES_THREAD_OK_V1);
    assert(v == NULL);
    assert(ores_native_thread_local_set_v1(1, new_value(a->idx), drop_value) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_local_get_v1(1, &v) == ORES_THREAD_OK_V1);
    assert(*(int *)v == a->idx);
    assert(ores_native_thread_local_set_v1(1, v, drop_value) == ORES_THREAD_BAD_ARGUMENT_V1);
    assert(ores_native_thread_local_set_v1(1, new_value(a->idx+10), drop_value) == ORES_THREAD_OK_V1);
    assert(ores_native_thread_local_get_v1(1, &v) == ORES_THREAD_OK_V1);
    assert(*(int *)v == a->idx+10);
    assert(ores_native_thread_local_set_v1(2, new_value(55), drop_value) == ORES_THREAD_OK_V1);
    ores_native_thread_drop_v1 drop = NULL;
    assert(ores_native_thread_local_take_v1(2, &v, &drop) == ORES_THREAD_OK_V1);
    assert(v && *(int *)v == 55 && drop == drop_value);
    drop(v);
    assert(ores_native_thread_local_get_v1(2, &v) == ORES_THREAD_OK_V1);
    assert(v == NULL);
    atomic_fetch_add(&entry_count, 1);
    return 42+a->idx;
}
int main(void) {
    ores_native_thread_v1 *handles[4] = {0};
    void *outside = (void *)1;
    uint64_t id = 0;
    assert(ores_native_thread_local_get_v1(1, &outside) == ORES_THREAD_NO_CONTEXT_V1);
    assert(outside == NULL);
    assert(ores_native_thread_id_v1(&id) == ORES_THREAD_NO_CONTEXT_V1);
    assert(ores_native_thread_spawn_v1(NULL, NULL, NULL, &handles[0]) == ORES_THREAD_BAD_ARGUMENT_V1);
    int *invalid_arg = new_value(11);
    assert(ores_native_thread_spawn_v1(worker, invalid_arg, NULL, &handles[0]) == ORES_THREAD_BAD_ARGUMENT_V1);
    free(invalid_arg); /* spawn failure retained caller ownership */
    assert(ores_native_thread_poll_completed_v1(NULL, NULL) == ORES_THREAD_BAD_ARGUMENT_V1);
    for (int i = 0; i < 4; ++i) {
        args *a = malloc(sizeof(*a));
        assert(a);
        a->idx = i;
        assert(ores_native_thread_spawn_v1(worker, a, drop_value, &handles[i]) == ORES_THREAD_OK_V1);
    }
    for (int i = 0; i < 4; ++i) {
        int result = 0;
        int completed = -1;
        assert(ores_native_thread_poll_completed_v1(handles[i], &completed) == ORES_THREAD_OK_V1);
        assert(completed == 0 || completed == 1);
        assert(ores_native_thread_join_blocking_v1(handles[i], &result) == ORES_THREAD_OK_V1);
        assert(ores_native_thread_poll_completed_v1(handles[i], &completed) == ORES_THREAD_OK_V1);
        assert(completed == 1);
        assert(result == 42+i);
        assert(ores_native_thread_join_blocking_v1(handles[i], &result) == ORES_THREAD_ALREADY_JOINED_V1);
        assert(ores_native_thread_release_v1(handles[i]) == ORES_THREAD_OK_V1);
    }
    for (int i = 0; i < 4; ++i)
        for (int j = i+1; j < 4; ++j)
            assert(atomic_load(&seen_ids[i]) != atomic_load(&seen_ids[j]));
    assert(atomic_load(&entry_count) == 4);
    /* per worker: replaced slot, taken slot, remaining slot, moved arg */
    assert(atomic_load(&drops) == 16);
#ifdef ORES_NATIVE_THREAD_TEST_ID_EXHAUSTION
    /* Reserve the final usable incarnation, then permanently exhaust IDs.
     * A failed spawn must leave the caller's owned capture intact. */
    ores_native_thread_test_set_next_id_v1(UINT64_MAX - 1);
    args *final_arg = malloc(sizeof(*final_arg));
    assert(final_arg); final_arg->idx = 0;
    ores_native_thread_v1 *final_handle = NULL;
    assert(ores_native_thread_spawn_v1(
        worker, final_arg, drop_value, &final_handle) == ORES_THREAD_OK_V1);
    int final_result = -1;
    assert(ores_native_thread_join_blocking_v1(
        final_handle, &final_result) == ORES_THREAD_OK_V1);
    assert(final_result == 42);
    assert(ores_native_thread_release_v1(final_handle) == ORES_THREAD_OK_V1);
    args *rejected_arg = malloc(sizeof(*rejected_arg));
    assert(rejected_arg); rejected_arg->idx = 0;
    ores_native_thread_v1 *rejected_handle = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_spawn_v1(
        worker, rejected_arg, drop_value, &rejected_handle) == ORES_THREAD_OS_ERROR_V1);
    assert(rejected_handle == NULL);
    free(rejected_arg); /* failed admission did not consume ownership */
    rejected_handle = (ores_native_thread_v1 *)1;
    assert(ores_native_thread_spawn_v1(
        worker, NULL, NULL, &rejected_handle) == ORES_THREAD_OS_ERROR_V1);
    assert(rejected_handle == NULL); /* permanent exhaustion, no ID reuse */
    assert(atomic_load(&drops) == 20);
#endif
    puts("native pthread substrate: OK (host-only; no guest compiler claim)");
    return 0;
}

