#include "oreslang/runtime/SourceTaskRuntime.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic(uintptr_t) published_task = 0;
static _Atomic(int) carrier_await_status = -1;
static _Atomic(int) tasklocal_producer_ready = 0;
static _Atomic(int) tasklocal_producer_release = 0;

typedef struct {
    void* task;
    _Atomic(int)* go;
    int status;
} DuplicateAwaitContext;

static int64_t simple_producer(void) {
    int64_t missing = -1;
    assert(!oreslang_source_task_local_i64_get_checked(&missing));
    assert(missing == -1);
    assert(!oreslang_source_task_local_i64_get_slot_checked(7u, &missing));
    assert(missing == -1);
    return 7;
}

static int64_t tasklocal_producer(void) {
    int64_t missing = -1;
    assert(!oreslang_source_task_local_i64_get_checked(&missing));
    assert(oreslang_source_task_local_i64_set_checked(41));
    assert(oreslang_source_task_local_i64_set_slot_checked(1u, -9));
    assert(oreslang_source_task_local_i64_set_slot_checked(7u, 99));
    assert(!oreslang_source_task_local_i64_set_slot_checked(
        ORES_SOURCE_TASK_LOCAL_I64_SLOTS, 123));
    int64_t keyed = 0;
    assert(oreslang_source_task_local_i64_get_slot_checked(1u, &keyed));
    assert(keyed == -9);
    assert(oreslang_source_task_local_i64_clear_slot_checked(1u));
    assert(!oreslang_source_task_local_i64_clear_slot_checked(1u));
    assert(!oreslang_source_task_local_i64_get_slot_checked(1u, &keyed));
    assert(!oreslang_source_task_local_i64_get_slot_checked(
        ORES_SOURCE_TASK_LOCAL_I64_SLOTS, &keyed));
    assert(!oreslang_source_task_local_i64_clear_slot_checked(
        ORES_SOURCE_TASK_LOCAL_I64_SLOTS));
    assert(oreslang_source_task_local_i64_get_slot_checked(7u, &keyed));
    assert(keyed == 99);

    // Compiler-issued i64 descriptors are separate from raw host slots.
    // Once a slot binds to a key, the type/key tag cannot be changed even
    // after a clear; missing, wrong-kind and foreign key reads fail closed.
    const uint64_t key_a =
        ORES_SOURCE_TASK_LOCAL_I64_TAG_PREFIX | UINT64_C(0x1234);
    const uint64_t key_b =
        ORES_SOURCE_TASK_LOCAL_I64_TAG_PREFIX | UINT64_C(0x5678);
    assert(!oreslang_source_task_local_i64_get_keyed_checked(
        2u, key_a, &keyed));
    assert(!oreslang_source_task_local_i64_set_keyed_checked(
        2u, UINT64_C(0x1234), 44));
    assert(!oreslang_source_task_local_i64_set_keyed_checked(
        2u, ORES_SOURCE_TASK_LOCAL_I64_TAG_PREFIX, 44));
    assert(!oreslang_source_task_local_i64_set_keyed_checked(
        ORES_SOURCE_TASK_LOCAL_I64_SLOTS, key_a, 44));
    assert(oreslang_source_task_local_i64_set_keyed_checked(2u, key_a, -44));
    assert(oreslang_source_task_local_i64_set_keyed_checked(3u, key_b, 123));
    assert(!oreslang_source_task_local_i64_set_slot_checked(2u, 11));
    assert(!oreslang_source_task_local_i64_get_slot_checked(2u, &keyed));
    assert(!oreslang_source_task_local_i64_clear_slot_checked(2u));
    assert(!oreslang_source_task_local_i64_set_keyed_checked(2u, key_b, 44));
    assert(!oreslang_source_task_local_i64_get_keyed_checked(
        2u, key_b, &keyed));
    assert(oreslang_source_task_local_i64_get_keyed_checked(
        2u, key_a, &keyed));
    assert(keyed == -44);
    assert(oreslang_source_task_local_i64_get_keyed_checked(
        3u, key_b, &keyed));
    assert(keyed == 123);
    assert(!oreslang_source_task_local_i64_clear_keyed_checked(2u, key_b));
    assert(oreslang_source_task_local_i64_clear_keyed_checked(2u, key_a));
    assert(!oreslang_source_task_local_i64_clear_keyed_checked(2u, key_a));
    assert(!oreslang_source_task_local_i64_set_keyed_checked(2u, key_b, 12));
    assert(oreslang_source_task_local_i64_set_keyed_checked(2u, key_a, 12));
    assert(oreslang_source_task_local_i64_get_keyed_checked(
        2u, key_a, &keyed));
    assert(keyed == 12);
    // Keep a real scheduler turn active while a foreign pthread attempts to
    // spoof the TaskLocal accessor with an otherwise valid native symbol.
    atomic_store_explicit(
        &tasklocal_producer_ready, 1, memory_order_release);
    while (!atomic_load_explicit(
               &tasklocal_producer_release, memory_order_acquire))
        sched_yield();
    int64_t value = 0;
    assert(oreslang_source_task_local_i64_get_checked(&value));
    assert(value == 41);
    // The compiler's checked guest intrinsics are identical to these wrappers.
    oreslang_source_task_local_i64_set(42);
    return oreslang_source_task_local_i64_get();
}

static void* duplicate_await(void* opaque) {
    DuplicateAwaitContext* context = (DuplicateAwaitContext*)opaque;
    while (!atomic_load_explicit(context->go, memory_order_acquire))
        sched_yield();
    int64_t ignored = 0;
    context->status = oreslang_source_task_await_i64_checked(
        context->task, &ignored);
    return NULL;
}

static int64_t producer(void) {
    uintptr_t raw = 0;
    while ((raw = atomic_load_explicit(
                &published_task, memory_order_acquire)) == 0)
        sched_yield();

    int64_t impossible = 0;
    const int status = oreslang_source_task_await_i64_checked(
        (void*)raw, &impossible);
    atomic_store_explicit(
        &carrier_await_status, status, memory_order_release);
    return 41;
}

int main(void) {
    void* task = 0;
    assert(oreslang_source_task_start_i64_checked(producer, &task)
        == ORES_SOURCE_TASK_OK);
    assert(task != 0);
    atomic_store_explicit(
        &published_task, (uintptr_t)task, memory_order_release);

    int64_t result = 0;
    assert(oreslang_source_task_await_i64_checked(task, &result)
        == ORES_SOURCE_TASK_OK);
    assert(result == 41);
    assert(atomic_load_explicit(
        &carrier_await_status, memory_order_acquire)
        == ORES_SOURCE_TASK_WOULD_DEADLOCK);

    int64_t duplicate = 0;
    assert(oreslang_source_task_await_i64_checked(task, &duplicate)
        == ORES_SOURCE_TASK_ALREADY_CONSUMED);

    assert(oreslang_source_task_runtime_shutdown() == 1);
    assert(oreslang_source_task_runtime_shutdown() == 1);

    // No host thread may access the retired TaskLocal slot, even when it
    // retains the typed function symbol. Next task begins with no value.
    int64_t forbidden = -1;
    assert(!oreslang_source_task_local_i64_set_checked(4));
    assert(!oreslang_source_task_local_i64_get_checked(&forbidden));
    assert(!oreslang_source_task_local_i64_get_checked(NULL));
    void* owned = NULL;
    assert(oreslang_source_task_start_i64_checked(tasklocal_producer, &owned)
        == ORES_SOURCE_TASK_OK);
    while (!atomic_load_explicit(
               &tasklocal_producer_ready, memory_order_acquire))
        sched_yield();
    // A premature external shutdown MUST fail without closing the active
    // producer: its next authenticated TaskLocal read/write must still work.
    // Before the fix shutdown set runtime.closing=1 while awaiting the task,
    // so the producer's checked TaskLocal calls failed or aborted.
    assert(oreslang_source_task_runtime_shutdown() == 0);
    assert(oreslang_source_task_runtime_shutdown() == 0);
    void* rejected_concurrent = NULL;
    assert(oreslang_source_task_start_i64_checked(
        simple_producer, &rejected_concurrent)
        == ORES_SOURCE_TASK_RESOURCE_EXHAUSTED);
    assert(rejected_concurrent == NULL);
    // Reject foreign access WHILE the right Task is actively running.
    assert(!oreslang_source_task_local_i64_set_checked(99));
    assert(!oreslang_source_task_local_i64_get_checked(&forbidden));
    assert(!oreslang_source_task_local_i64_set_slot_checked(7u, 100));
    assert(!oreslang_source_task_local_i64_get_slot_checked(7u, &forbidden));
    assert(!oreslang_source_task_local_i64_clear_slot_checked(7u));
    const uint64_t foreign_key =
        ORES_SOURCE_TASK_LOCAL_I64_TAG_PREFIX | UINT64_C(0x1234);
    assert(!oreslang_source_task_local_i64_set_keyed_checked(
        2u, foreign_key, 7));
    assert(!oreslang_source_task_local_i64_get_keyed_checked(
        2u, foreign_key, &forbidden));
    assert(!oreslang_source_task_local_i64_clear_keyed_checked(
        2u, foreign_key));
    atomic_store_explicit(
        &tasklocal_producer_release, 1, memory_order_release);
    int64_t owned_result = 0;
    assert(oreslang_source_task_await_i64_checked(owned, &owned_result)
        == ORES_SOURCE_TASK_OK);
    assert(owned_result == 42);
    assert(!oreslang_source_task_local_i64_set_checked(42));
    assert(!oreslang_source_task_local_i64_get_checked(&forbidden));
    assert(oreslang_source_task_runtime_shutdown() == 1);

    /*
     * Race duplicate post-consumption await admission against physical
     * shutdown/reclamation. The duplicate may enter before the closing fence
     * (ALREADY_CONSUMED) or after it (INVALID), but it must never touch freed
     * task synchronization state.
     */
    for (int i = 0; i < 64; ++i) {
        void* raced_task = NULL;
        assert(oreslang_source_task_start_i64_checked(
            simple_producer, &raced_task) == ORES_SOURCE_TASK_OK);
        int64_t raced_result = 0;
        assert(oreslang_source_task_await_i64_checked(
            raced_task, &raced_result) == ORES_SOURCE_TASK_OK);
        assert(raced_result == 7);

        _Atomic(int) go = 0;
        DuplicateAwaitContext context = {
            .task = raced_task,
            .go = &go,
            .status = -1
        };
        pthread_t duplicate;
        assert(pthread_create(
            &duplicate, NULL, duplicate_await, &context) == 0);
        atomic_store_explicit(&go, 1, memory_order_release);

        assert(oreslang_source_task_runtime_shutdown() == 1);
        assert(pthread_join(duplicate, NULL) == 0);
        assert(context.status == ORES_SOURCE_TASK_ALREADY_CONSUMED ||
            context.status == ORES_SOURCE_TASK_INVALID);
    }

    assert(oreslang_source_task_runtime_shutdown() == 1);
    return 0;
}
