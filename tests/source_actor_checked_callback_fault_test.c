/* Host C test of the NEW source-actor checked callback/ABI boundary.
 * Real .atto codegen must be tested independently through oreslang-llvmc;
 * this test exercises runtime admission, fault retirement, sibling isolation
 * and physically-executed versus successfully-completed DATA accounting.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <inttypes.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_uint success_a;
static atomic_uint success_b;
static atomic_uint fault_a;

static void check(int condition) {
    if (!condition) abort();
}

static void actor_init(OresActorI64Heap* heap,
                       uint64_t domain, uint64_t epoch) {
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 40);
}

/* Compiler-equivalent checked output ABI, but still a host C callback.
 * payload 2 deliberately issues an invalid field read to return BAD_HANDLE.
 * Other operations read/write one private actor field through checked APIs.
 */
static int checked_message(OresActorI64Heap* heap, uint64_t domain,
                           uint64_t epoch, int64_t payload,
                           int64_t* result) {
    if (payload == 2) {
        const int fault = ores_actor_i64_heap_try_field_read(
            heap, domain, epoch, UINT64_MAX, result);
        if (fault == ORES_ACTOR_LOAN_BAD_HANDLE)
            atomic_fetch_add_explicit(&fault_a, 1, memory_order_acq_rel);
        return fault;
    }
    int64_t value = 0;
    int status = ores_actor_i64_heap_try_field_read(
        heap, domain, epoch, 0, &value);
    if (status != ORES_ACTOR_LOAN_OK) return status;
    status = ores_actor_i64_heap_try_field_write(
        heap, domain, epoch, 0, value + payload);
    if (status != ORES_ACTOR_LOAN_OK) return status;
    *result = value + payload;
    if (payload < 10)
        atomic_fetch_add_explicit(&success_a, 1, memory_order_acq_rel);
    else
        atomic_fetch_add_explicit(&success_b, 1, memory_order_acq_rel);
    return ORES_ACTOR_LOAN_OK;
}

int main(void) {
    void* failed = oreslang_source_actor_spawn_checked(
        actor_init, checked_message, 1);
    void* sibling = oreslang_source_actor_spawn_checked(
        actor_init, checked_message, 1);
    check(failed != NULL && sibling != NULL);

    oreslang_source_actor_send(failed, 1);
    oreslang_source_actor_send(failed, 2);
    oreslang_source_actor_send(failed, 3); /* must be abandoned after fault */
    oreslang_source_actor_send(sibling, 10);

    for (unsigned spins = 0; spins < 3000000; ++spins) {
        if (oreslang_source_actor_fault_status(failed)
                == ORES_ACTOR_LOAN_BAD_HANDLE
            && oreslang_source_actor_successful_messages(sibling) == 1)
            break;
        sched_yield();
    }
    check(oreslang_source_actor_fault_status(failed)
        == ORES_ACTOR_LOAN_BAD_HANDLE);
    check(oreslang_source_actor_successful_messages(failed) == 1);
    check(oreslang_source_actor_failed_messages(failed) == 1);
    check(atomic_load_explicit(&fault_a, memory_order_acquire) == 1);
    check(atomic_load_explicit(&success_a, memory_order_acquire) == 1);
    check(oreslang_source_actor_successful_messages(sibling) == 1);
    check(oreslang_source_actor_failed_messages(sibling) == 0);

    /* The failed actor has already requested a sealed stop. The trusted
     * main's matching stop request must be idempotent, never a second stop
     * that aborts or revives an abandoned mailbox message. */
    oreslang_source_actor_request_stop(failed);
    oreslang_source_actor_join_done(failed);

    /* Sibling still admits another DATA turn after the other heap retired. */
    oreslang_source_actor_send(sibling, 11);
    for (unsigned spins = 0; spins < 3000000; ++spins) {
        if (oreslang_source_actor_successful_messages(sibling) == 2)
            break;
        sched_yield();
    }
    check(oreslang_source_actor_successful_messages(sibling) == 2);
    check(oreslang_source_actor_failed_messages(sibling) == 0);
    check(atomic_load_explicit(&success_b, memory_order_acquire) == 2);
    oreslang_source_actor_request_stop(sibling);
    oreslang_source_actor_join_done(sibling);

    puts("SOURCE_ACTOR_CHECKED_CALLBACK_FAULT_PASS "
         "successful_a=1 failed_a=1 abandoned_a=1 "
         "successful_b=2 failed_b=0");
    puts("SOURCE_ACTOR_HOST_ONLY_NOT_ATTO_GUEST_OR_SUPERVISOR_PROOF");
    return 0;
}
