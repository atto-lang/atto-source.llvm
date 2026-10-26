#include "oreslang/runtime/SourceTaskRuntime.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic(uintptr_t) published_task = 0;
static _Atomic(int) carrier_await_status = -1;

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
    return 0;
}
