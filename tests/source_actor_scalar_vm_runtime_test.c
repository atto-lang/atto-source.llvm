#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/SourceActorScalarVM.h"
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int tail_processed;
static atomic_int quota_processed;
static atomic_int sibling_processed;
static atomic_int sibling_after_failure;
static atomic_int observed_quota_error;
static atomic_int malformed_processed;
static atomic_int bounded_processed;
static atomic_int tag_failure_processed;
static atomic_int fuel_failure_processed;
static atomic_int cancellation_processed;
static atomic_int tail_operands_denied;
static atomic_int scalar_arithmetic_verified;
static atomic_int nested_reentry_denied;

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
/* Preflight admits these valid opcodes; runtime must still reject a forged
 * typed operand (NOT on an i64), and a finite-fuel tight guest loop. */
static const OresActorScalarInstruction wrong_tag_code[] = {
    OP(ORES_SCALAR_PUSH_I64, 1),
    OP(ORES_SCALAR_NOT, 0),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram wrong_tag_program = {
    1, sizeof(wrong_tag_code)/sizeof(wrong_tag_code[0]),
    0, entry, wrong_tag_code
};
static const OresActorScalarInstruction infinite_code[] = {
    OP(ORES_SCALAR_PUSH_I64, 1),
    OP(ORES_SCALAR_PUSH_I64, 0),
    OP(ORES_SCALAR_LE, 0),
    OP(ORES_SCALAR_BRANCH_FALSE, 0),
    OP(ORES_SCALAR_PUSH_I64, 0),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram infinite_program = {
    1, sizeof(infinite_code)/sizeof(infinite_code[0]),
    0, entry, infinite_code
};
/* Tail transfer with live unrelated operands is an illegal cleanup escape. */
static const OresActorScalarInstruction invalid_tail_stack_code[] = {
    OP(ORES_SCALAR_PUSH_I64, 88),
    OP(ORES_SCALAR_PUSH_I64, 1),
    OP(ORES_SCALAR_TAIL_CALL, 0)
};
static const OresActorScalarProgram invalid_tail_stack_program = {
    1, sizeof(invalid_tail_stack_code)/sizeof(invalid_tail_stack_code[0]),
    0, entry, invalid_tail_stack_code
};
/* i64 arithmetic must wrap modulo 2^64; ordering remains signed. */
static const OresActorScalarInstruction wrapping_code[] = {
    OP(ORES_SCALAR_PUSH_I64, INT64_MAX),
    OP(ORES_SCALAR_PUSH_I64, 1),
    OP(ORES_SCALAR_ADD, 0),
    OP(ORES_SCALAR_PUSH_I64, INT64_MIN),
    OP(ORES_SCALAR_EQ, 0),
    OP(ORES_SCALAR_BRANCH_FALSE, 8),
    OP(ORES_SCALAR_PUSH_I64, 11),
    OP(ORES_SCALAR_RETURN, 0),
    OP(ORES_SCALAR_PUSH_I64, -1),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram wrapping_program = {
    1, sizeof(wrapping_code)/sizeof(wrapping_code[0]),
    0, entry, wrapping_code
};
static const OresActorScalarInstruction signed_code[] = {
    OP(ORES_SCALAR_PUSH_I64, INT64_MIN),
    OP(ORES_SCALAR_PUSH_I64, -1),
    OP(ORES_SCALAR_LT, 0),
    OP(ORES_SCALAR_BRANCH_FALSE, 6),
    OP(ORES_SCALAR_PUSH_I64, 12),
    OP(ORES_SCALAR_RETURN, 0),
    OP(ORES_SCALAR_PUSH_I64, -1),
    OP(ORES_SCALAR_RETURN, 0)
};
static const OresActorScalarProgram signed_program = {
    1, sizeof(signed_code)/sizeof(signed_code[0]),
    0, entry, signed_code
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

static int wait_checkpoint_entry(void) {
    struct timespec start, now;
    check(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    do {
        if (oreslang_source_actor_test_scalar_checkpoint_entered()) return 1;
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
    /* A caught quota fault poisons the actor for the rest of its physical
     * callback. It cannot restart an unaccounted scalar root. */
    out = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &echo_program, payload, &out) == ORES_SCALAR_INVALID_FRAME
        && out == 0);
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
static int64_t bounded_depth_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 63);
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &depth_program, payload, &result) == ORES_SCALAR_OK);
    check(result == 63);
    atomic_fetch_add_explicit(&bounded_processed, 1, memory_order_release);
    return result;
}
static int64_t wrong_tag_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &wrong_tag_program, 1, &result) == ORES_SCALAR_INVALID_FRAME
        && result == 0);
    atomic_fetch_add_explicit(
        &tag_failure_processed, 1, memory_order_release);
    return 0;
}
static int64_t fuel_exhausted_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &infinite_program, 1, &result) == ORES_SCALAR_FUEL_EXHAUSTED
        && result == 0);
    atomic_fetch_add_explicit(
        &fuel_failure_processed, 1, memory_order_release);
    return 0;
}

static int64_t nested_entry_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 1);
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &infinite_program, 1, &result) == ORES_SCALAR_INVALID_FRAME
        && result == 0);
    atomic_fetch_add_explicit(&nested_reentry_denied, 1,
        memory_order_release);
    return 0;
}

static int64_t invalid_tail_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &invalid_tail_stack_program, 1, &result) == ORES_SCALAR_INVALID_FRAME
        && result == 0);
    atomic_fetch_add_explicit(&tail_operands_denied, 1,
        memory_order_release);
    return 0;
}
static int64_t arithmetic_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain && epoch && payload == 1);
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &wrapping_program, 0, &result) == ORES_SCALAR_OK && result == 11);
    /* VM active gate releases after a successful root call; a second root
     * may safely reuse the same scrubbed actor-owned frame allocation. */
    result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &signed_program, 0, &result) == ORES_SCALAR_OK && result == 12);
    atomic_fetch_add_explicit(&scalar_arithmetic_verified, 1,
        memory_order_release);
    return result;
}
static int64_t cancelled_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t result = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &infinite_program, 1, &result) == ORES_SCALAR_CANCELLED
        && result == 0);
    atomic_fetch_add_explicit(&cancellation_processed, 1,
        memory_order_release);
    return 0;
}

static int64_t malformed_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; (void)domain; (void)epoch; (void)payload;
    int64_t out = -1;
    check(oreslang_source_actor_scalar_vm_execute(
        &invalid_program, 0, &out) == ORES_SCALAR_INVALID_PROGRAM && out == 0);
    atomic_fetch_add_explicit(
        &malformed_processed, 1, memory_order_release);
    return 0;
}

int main(void) {
    /* Invalid host protocol cannot silently acquire another stop claim.
     * Fork BEFORE native schedulers start (never fork live carrier threads),
     * disable core dumps, and assert the second outer stop is rejected. */
    const pid_t reject_double_stop = fork();
    check(reject_double_stop >= 0);
    if (reject_double_stop == 0) {
        const struct rlimit no_core = {0, 0};
        check(setrlimit(RLIMIT_CORE, &no_core) == 0);
        void* must_fail = oreslang_source_actor_spawn(
            init, malformed_message, 1);
        check(must_fail != NULL);
        oreslang_source_actor_send(must_fail, 1);
        check(wait_for(&malformed_processed, 1));
        oreslang_source_actor_request_stop(must_fail);
        /* An auto-failstop and a root-stop both confer only ONE outer claim. */
        oreslang_source_actor_request_stop(must_fail);
        _exit(123);
    }
    int child_status = 0;
    check(waitpid(reject_double_stop, &child_status, 0)
        == reject_double_stop);
    check(WIFSIGNALED(child_status)
        && WTERMSIG(child_status) == SIGABRT);

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
    check(fresh != NULL);
    oreslang_source_actor_send(fresh, 77);
    check(wait_for(&sibling_after_failure, 1));
    oreslang_source_actor_stop(fresh);

    /* Invalid program admission is also actor-local and terminal. */
    void* malformed = oreslang_source_actor_spawn(
        init, malformed_message, 1);
    check(malformed != NULL);
    oreslang_source_actor_send(malformed, 1);
    check(wait_for(&malformed_processed, 1));
    /* wait_done is trusted host-only; malformed actor must automatically
     * terminalize and never park a carrier on its own STOP. */
    oreslang_source_actor_request_stop(malformed);
    oreslang_source_actor_join_done(malformed);

    /* The last admitted non-tail depth is exactly 64 actor-owned frames,
     * so depth(63) must return cleanly before depth(64) would overflow. */
    void* boundary = oreslang_source_actor_spawn(
        init, bounded_depth_message, 1);
    check(boundary != NULL);
    oreslang_source_actor_send(boundary, 63);
    check(wait_for(&bounded_processed, 1));
    oreslang_source_actor_stop(boundary);

    void* wrong_tag = oreslang_source_actor_spawn(
        init, wrong_tag_message, 1);
    check(wrong_tag != NULL);
    oreslang_source_actor_send(wrong_tag, 1);
    check(wait_for(&tag_failure_processed, 1));
    oreslang_source_actor_stop(wrong_tag);

    void* infinite = oreslang_source_actor_spawn(
        init, fuel_exhausted_message, 1);
    check(infinite != NULL);
    oreslang_source_actor_send(infinite, 1);
    check(wait_for(&fuel_failure_processed, 1));
    oreslang_source_actor_stop(infinite);

    /* Deterministic cancellation while inside the VM: pause only the
     * *test-compiled* actor at a 256-instruction checkpoint. Healthy sibling
     * messages still run on another carrier, then an outer STOP releases the
     * offending actor. Production builds contain no slowpath test pause. */
    oreslang_source_actor_test_hold_scalar_checkpoint(1);
    void* cancelled = oreslang_source_actor_spawn(
        init, cancelled_message, 1);
    check(cancelled != NULL);
    oreslang_source_actor_send(cancelled, 1);
    check(wait_checkpoint_entry());
    check(oreslang_source_actor_test_scalar_checkpoint_count(cancelled) >= 1);
    oreslang_source_actor_send(sibling, 14);
    check(wait_for(&sibling_processed, 3));
    oreslang_source_actor_request_stop(cancelled);
    check(wait_for(&cancellation_processed, 1));
    /* Exact 256 dispatched instructions, not "at most 2M eventually".
     * Count excludes the next opcode, which cancellation never executed. */
    check(oreslang_source_actor_test_scalar_dispatched_steps(cancelled) == 256);
    check(oreslang_source_actor_test_scalar_checkpoint_count(cancelled) == 1);
    oreslang_source_actor_join_done(cancelled);
    oreslang_source_actor_test_hold_scalar_checkpoint(0);

    /* Mutation at the 256-op checkpoint attempts to recursively enter the
     * interpreter on the SAME actor. It must never overwrite live PC/slots. */
    oreslang_source_actor_test_reenter_scalar_vm_once();
    void* reentrant = oreslang_source_actor_spawn(
        init, nested_entry_message, 1);
    check(reentrant != NULL);
    oreslang_source_actor_send(reentrant, 1);
    check(wait_for(&nested_reentry_denied, 1));
    oreslang_source_actor_stop(reentrant);

    void* tail_bad = oreslang_source_actor_spawn(
        init, invalid_tail_message, 1);
    check(tail_bad != NULL);
    oreslang_source_actor_send(tail_bad, 1);
    check(wait_for(&tail_operands_denied, 1));
    oreslang_source_actor_stop(tail_bad);

    void* arithmetic = oreslang_source_actor_spawn(
        init, arithmetic_message, 1);
    check(arithmetic != NULL);
    oreslang_source_actor_send(arithmetic, 1);
    check(wait_for(&scalar_arithmetic_verified, 1));
    oreslang_source_actor_stop(arithmetic);

    /* The same sibling accepts another message after all three kinds of
     * actor-local failure: quota, malformed program, and typed tag/fuel. */
    oreslang_source_actor_send(sibling, 12);
    check(wait_for(&sibling_processed, 4));
    oreslang_source_actor_stop(tail);
    oreslang_source_actor_stop(sibling);
    puts("SOURCE_ACTOR_TYPED_SCALAR_VM_PASS tail=100000 quota=64 fuel=2000000 cancellation=256 sibling_survives=1");
    return 0;
}
