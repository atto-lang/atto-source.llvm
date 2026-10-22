#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "actor heap check failed at %d\n", __LINE__); return 1; } } while (0)

enum Misuse { WRONG_OWNER, WRONG_EPOCH, FORGED_SLOT, DROP_LOANED, WRITE_LOANED, STALE_SLOT_REUSED, FOREIGN_SLOT, DOUBLE_LOAN_RELEASE, REENTER, CALL_AFTER_RETIRE };
static void invalid_call(enum Misuse which) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(29, 7, 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 29, 7);
    OresActorSlot slot = ores_actor_i64_heap_new(heap, turn, 99);
    if (which == WRONG_OWNER) {
        ores_actor_i64_heap_leave(heap, turn);
        (void) ores_actor_i64_heap_enter(heap, 30, 7);
    } else if (which == WRONG_EPOCH) {
        ores_actor_i64_heap_leave(heap, turn);
        OresActorTurn current = ores_actor_i64_heap_enter(heap, 29, 7);
        (void) current;
        (void) ores_actor_i64_heap_read(heap, turn, slot);
    } else if (which == FORGED_SLOT) {
        slot.domain++;
        (void) ores_actor_i64_heap_read(heap, turn, slot);
    } else if (which == DROP_LOANED) {
        (void) ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_drop(heap, turn, slot);
    } else if (which == WRITE_LOANED) {
        (void) ores_actor_i64_heap_borrow(heap, turn, slot);
        /* Even a different admitted turn must not mutate shared-borrowed data. */
        ores_actor_i64_heap_leave(heap, turn);
        OresActorTurn resumed = ores_actor_i64_heap_enter(heap, 29, 7);
        ores_actor_i64_heap_write(heap, resumed, slot, 22);
    } else if (which == STALE_SLOT_REUSED) {
        ores_actor_i64_heap_drop(heap, turn, slot);
        (void) ores_actor_i64_heap_new(heap, turn, 12);
        (void) ores_actor_i64_heap_read(heap, turn, slot);
    } else if (which == FOREIGN_SLOT) {
        OresActorI64Heap* foreign = ores_actor_i64_heap_open(30, 7, 2);
        OresActorTurn other = ores_actor_i64_heap_enter(foreign, 30, 7);
        OresActorSlot other_slot = ores_actor_i64_heap_new(foreign, other, 5);
        (void) ores_actor_i64_heap_read(heap, turn, other_slot);
    } else if (which == DOUBLE_LOAN_RELEASE) {
        OresActorLoan loan = ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_release(heap, loan);
        ores_actor_i64_heap_release(heap, loan);
    } else if (which == REENTER) {
        (void) ores_actor_i64_heap_enter(heap, 29, 7);
    } else if (which == CALL_AFTER_RETIRE) {
        ores_actor_i64_heap_retire(heap);
        (void) ores_actor_i64_heap_new(heap, turn, 6);
    }
    /* None of these calls may finish normally. */
}
#if defined(__unix__) || defined(__APPLE__)
static int rejects(enum Misuse which) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void) setrlimit(RLIMIT_CORE, &limit);
        invalid_call(which);
        _exit(91);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif
typedef struct {
    OresActorI64Heap* heap;
    uint64_t expected;
    uint64_t updated;
    int failed;
} Carrier;
static int second_carrier(void* argument) {
    Carrier* carrier = (Carrier*) argument;
    OresActorTurn turn = ores_actor_i64_heap_enter(carrier->heap, 29, 7);
    OresActorSlot slot = ores_actor_i64_heap_new(carrier->heap, turn, (int64_t) carrier->expected);
    if (ores_actor_i64_heap_read(carrier->heap, turn, slot) != (int64_t) carrier->expected) carrier->failed = 1;
    ores_actor_i64_heap_write(carrier->heap, turn, slot, (int64_t) carrier->updated);
    if (ores_actor_i64_heap_read(carrier->heap, turn, slot) != (int64_t) carrier->updated) carrier->failed = 1;
    ores_actor_i64_heap_drop(carrier->heap, turn, slot);
    ores_actor_i64_heap_leave(carrier->heap, turn);
    return 0;
}

int main(void) {
    /* One actor heap survives between logical turns and native carriers. */
    OresActorI64Heap* heap = ores_actor_i64_heap_open(29, 7, 2);
    OresActorTurn first = ores_actor_i64_heap_enter(heap, 29, 7);
    OresActorSlot saved = ores_actor_i64_heap_new(heap, first, 40);
    CHECK(ores_actor_i64_heap_read(heap, first, saved) == 40);
    OresActorLoan suspended = ores_actor_i64_heap_borrow(heap, first, saved);
    ores_actor_i64_heap_leave(heap, first); /* simulates await/suspension */
    OresActorTurn next = ores_actor_i64_heap_enter(heap, 29, 7);
    CHECK(ores_actor_i64_heap_read(heap, next, saved) == 40);
    ores_actor_i64_heap_leave(heap, next); /* shared loan still protects saved */
    /* A different native thread executes the next actor turn. */
    Carrier carrier = {heap, 21, 43, 0};
    int exit_status = -1;
    std::thread native([&] { exit_status = second_carrier(&carrier); });
    native.join();
    CHECK(exit_status == 0 && !carrier.failed);
    OresActorTurn after = ores_actor_i64_heap_enter(heap, 29, 7);
    CHECK(ores_actor_i64_heap_read(heap, after, saved) == 40);
    ores_actor_i64_heap_leave(heap, after);
    /* End the read-only lease before admitting an exclusive write. */
    ores_actor_i64_heap_release(heap, suspended);
    OresActorTurn writable = ores_actor_i64_heap_enter(heap, 29, 7);
    ores_actor_i64_heap_write(heap, writable, saved, 42);
    CHECK(ores_actor_i64_heap_read(heap, writable, saved) == 42);
    OresActorLoan shutdown_hold = ores_actor_i64_heap_borrow(heap, writable, saved);
    ores_actor_i64_heap_leave(heap, writable);
    ores_actor_i64_heap_retire(heap);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0); /* outstanding loan */
    ores_actor_i64_heap_release(heap, shutdown_hold);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
#if defined(__unix__) || defined(__APPLE__)
    CHECK(rejects(WRONG_OWNER));
    CHECK(rejects(WRONG_EPOCH));
    CHECK(rejects(FORGED_SLOT));
    CHECK(rejects(DROP_LOANED));
    CHECK(rejects(WRITE_LOANED));
    CHECK(rejects(STALE_SLOT_REUSED));
    CHECK(rejects(FOREIGN_SLOT));
    CHECK(rejects(DOUBLE_LOAN_RELEASE));
    CHECK(rejects(REENTER));
    CHECK(rejects(CALL_AFTER_RETIRE));
#endif
    return 0;
}
