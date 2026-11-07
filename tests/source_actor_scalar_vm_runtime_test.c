#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/SourceActorScalarVM.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

static atomic_int tail_processed;
static atomic_int quota_processed;
static atomic_int sibling_processed;
static atomic_int sibling_after_failure;
static atomic_int observed_quota_error;

#define OP(x, y) { x, y }
/* tail(n) = n <= 0 ? 0 : tail(n-1). Executes 100k recursive guest
 * transfers while the native/C call depth remains constant. */
static const OresActorScalarInstruction tail_code[] = {
    OP(ORES_SCALAR_LOAD_ARG, 0),           /* 0 */
    OP(ORES_SCALAR_PUSH_I64, 0),           /* 1 */
    OP(ORES_SCALAR_LE, 0),                 /* 2 */
    OP(ORES_SCALAR_BRANCH_FALSE, 6),      /* 3 */
    OP(ORES_SCALAR_PUSH_I64, 0),           /* 4 */
    OP(ORES_SCALAR_RETURN, 0),            /* 5 */
    OP(ORES_SCALAR_LOAD_ARG, 0),           /* 6 */
    OP(ORES_SCALAR_PUSH_I64, 1),           /* 7 */
    OP(ORES_SCALAR_SUB, 0),                /* 8 */
    OP(ORES_SCALAR_TAIL_CALL, 0),          /* 9 */
};
static const uint64_t entry[] = {0};
static const OresActorScalarProgram tail_program = {
    1, sizeof(tail_code)/sizeof(tail_code[0]), 0, entry, tail_code
};
/* depth(n) = n <= 0 ? 0 : 1 + depth(n-1). Non-tail recursive
 * CALL must exhaust the actor's bounded 64-frame quota without
 * destroying the process or another actor's mailbox. */
static const OresActorScalarInstruction depth_code[] = {
    OP(ORES_SCALAR_LOAD_ARG, 0),           /* 0 */
    OP(ORES_SCALAR_PUSH_I64, 0),           /* 1 */
    OP(ORES_SCALAR_LE, 0),                 /* 2 */
    OP(ORES_SCALAR_BRANCH_FALSE, 6),      /* 3 */
    OP(ORES_SCALAR_PUSH_I64, 0),           /* 4 */
    OP(ORES_SCALAR_RETURN, 0),            /* 5 */
    OP(ORES_SCALAR_PUSH_I64, 1),           /* 6 */
    OP(ORES_SCALAR_LOAD_ARG, 0),           /* 7 */
    OP(ORES_SCALAR_PUSH_I64, 1),           /* 8 */
    OP(ORES_SCALAR_SUB, 0),                /* 9 */
    OP(ORES_SCALAR_CALL, 0),               /* 10 */
    OP(ORES_SCALAR_ADD, 0),                /* 11 */
    OP(ORES_SCALAR_RETURN, 0),            /* 12 */
};
static const OresActorScalarProgram depth_program = {
    1, sizeof(depth_code)/sizeof(depth_code[0]), 0, entry, depth_code
};
static const OresActorScalarInstruction echo_code[] = {
    OP(ORES_SCALAR_LOAD_ARG, 0),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram echo_program = {
    1, sizeof(echo_code)/sizeof(echo_code[0]), 0, entry, echo_code
};
/* Mutation witnesses: malformed opcodes/branch targets must be rejected
 * before the first interpreter instruction mutates actor frame bytes. */
static const OresActorScalarInstruction invalid_code[] = {
    OP(ORES_SCALAR_BRANCH_FALSE, 99),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram invalid_program = {
    1, sizeof(invalid_code)/sizeof(invalid_code[0]), 0, entry, invalid_code
};

static void check(int condition) { if (!condition) abort(); }

static int wait_for(atomic_int* counter, int target) {
    struct timespec start, now;
    check(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    do {
        if (atomic_load_explicit(counter, memory_order_acquire) >= target)
            return 1;
        sched_yield();
        check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    } while (now.tv_sec - start.tv_sec < 10);
    return 0;
}

static void init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain && epoch);
}
static int64_t tail_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch);
    int64_t out = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &tail_program, payload, &out) == ORES_SCALAR_OK);
    check(out == 0);
    atomic_fetch_add_explicit(&tail_processed, 1, memory_order_release);
    return out;
}
static int64_t quota_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch);
    int64_t out = -1;
    const int status = oreslang_source_actor_scalar_vm_execute(
        &depth_program, payload, &out);
    check(status == ORES_SCALAR_FRAME_QUOTA && out == 0);
    atomic_store_explicit(&observed_quota_error, status, memory_order_release);
    atomic_fetch_add_explicit(&quota_processed, 1, memory_order_release);
    return out;
}
static int64_t echo_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch);
    int64_t out = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &echo_program, payload, &out) == ORES_SCALAR_OK);
    check(out == payload);
    atomic_fetch_add_explicit(&sibling_processed, 1, memory_order_release);
    return out;
}
static int64_t post_failure_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    int64_t value = echo_message(heap, domain, epoch, payload);
    atomic_fetch_add_explicit(
        &sibling_after_failure, 1, memory_order_release);
    return value;
}
static int64_t malformed_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t out = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &invalid_program, 0, &out) == ORES_SCALAR_INVALID_PROGRAM && out == 0);
    return 0;
}

int main(void) {
    int64_t outsider = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &tail_program, 1, &outsider) == ORES_SCALAR_INVALID_FRAME
        && outsider == 0);

    void* tail = oreslang_source_actor_spawn(init, tail_message, 1);
    void* quota = oreslang_source_actor_spawn(init, quota_message, 1);
    void* sibling = oreslang_source_actor_spawn(init, echo_message, 1);
    check(tail && quota && sibling);

    oreslang_source_actor_send(tail, 100000);
    oreslang_source_actor_send(quota, 100);
    oreslang_source_actor_send(sibling, 7);
    check(wait_for(&tail_processed, 1));
    check(wait_for(&quota_processed, 1));
    check(wait_for(&sibling_processed, 1));
    check(atomic_load_explicit(&observed_quota_error, memory_order_acquire)
        == ORES_SCALAR_FRAME_QUOTA);

    /* A failed actor must be joinable: the scheduler sealed its mailbox,
     * but explicit outer ownership still performs quiescent reclamation. */
    oreslang_source_actor_request_stop(quota);
    oreslang_source_actor_join_done(quota);
    oreslang_source_actor_send(sibling, 9);
    check(wait_for(&sibling_processed, 2));

    /* A fresh actor after failure runs on a functioning carrier and heap. */
    void* fresh = oreslang_source_actor_spawn(
        init, post_failure_message, 1);
    check(fresh);
    oreslang_source_actor_send(fresh, 77);
    check(wait_for(&sibling_after_failure, 1));
    oreslang_source_actor_stop(fresh);

    /* Invalid program admission is also actor-local and terminal. */
    void* malformed = oreslang_source_actor_spawn(
        init, malformed_message, 1);
    check(malformed);
    oreslang_source_actor_send(malformed, 1);
    /* wait_done is trusted host-only; malformed actor must automatically
     * terminalize and never park a carrier on its own STOP. */
    oreslang_source_actor_request_stop(malformed);
    oreslang_source_actor_join_done(malformed);

    oreslang_source_actor_stop(tail);
    oreslang_source_actor_stop(sibling);
    puts("SOURCE_ACTOR_TYPED_SCALAR_VM_PASS tail=100000 quota=64 siblings=2");
    return 0;
}
