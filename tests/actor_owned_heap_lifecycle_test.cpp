#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "actor heap check failed at %d\n", __LINE__); return 1; } } while (0)

enum Misuse { WRONG_OWNER, WRONG_EPOCH, FORGED_SLOT, DROP_LOANED, WRITE_LOANED, STALE_SLOT_REUSED, FOREIGN_SLOT, DOUBLE_LOAN_RELEASE, DOUBLE_EXTERNAL_PIN_RELEASE, REENTER, CALL_AFTER_RETIRE, FOREIGN_CARRIER_TURN, NESTED_CARRIER_ACTOR };
static void invalid_call(enum Misuse which) {
    OresActorSlot foreign_slot = {0, 0, 0};
    if (which == FOREIGN_SLOT) {
        OresActorI64Heap* foreign = ores_actor_i64_heap_open(30, 7, 2);
        OresActorTurn foreign_turn = ores_actor_i64_heap_enter(foreign, 30, 7);
        foreign_slot = ores_actor_i64_heap_new(foreign, foreign_turn, 5);
        ores_actor_i64_heap_leave(foreign, foreign_turn);
    }
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
        (void) ores_actor_i64_heap_read(heap, turn, foreign_slot);
    } else if (which == DOUBLE_LOAN_RELEASE) {
        OresActorLoan loan = ores_actor_i64_heap_borrow(heap, turn, slot);
        ores_actor_i64_heap_release(heap, loan);
        ores_actor_i64_heap_release(heap, loan);
    } else if (which == REENTER) {
        (void) ores_actor_i64_heap_enter(heap, 29, 7);
    } else if (which == CALL_AFTER_RETIRE) {
        ores_actor_i64_heap_retire(heap);
        (void) ores_actor_i64_heap_new(heap, turn, 6);
    } else if (which == FOREIGN_CARRIER_TURN) {
        /* The compiler-facing fixed-field ABI must reject a current turn
         * copied onto a different native carrier. */
        ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 77);
        std::thread stolen([&] {
            (void) ores_actor_i64_heap_field_read(
                heap, turn.domain, turn.epoch, 1);
        });
        stolen.join();
    } else if (which == DOUBLE_EXTERNAL_PIN_RELEASE) {
        OresActorLifetimePin pin =
            ores_actor_i64_heap_pin_external(heap, turn);
        ores_actor_i64_heap_release_external(heap, pin);
        ores_actor_i64_heap_release_external(heap, pin);
    } else if (which == NESTED_CARRIER_ACTOR) {
        OresActorI64Heap* other = ores_actor_i64_heap_open(30, 8, 1);
        (void) ores_actor_i64_heap_enter(other, 30, 8);
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
    /* A slotless lifetime pin models a continuation/FFI/I/O completion.
     * It is registered on the actor carrier, then safely released elsewhere
     * after logical retirement without granting memory access. */
    OresActorI64Heap* completion_heap = ores_actor_i64_heap_open(31, 9, 1);
    OresActorTurn completion_turn =
        ores_actor_i64_heap_enter(completion_heap, 31, 9);
    OresActorLifetimePin completion_pin_a =
        ores_actor_i64_heap_pin_external(completion_heap, completion_turn);
    OresActorLifetimePin completion_pin_b =
        ores_actor_i64_heap_pin_external(completion_heap, completion_turn);
    ores_actor_i64_heap_leave(completion_heap, completion_turn);
    ores_actor_i64_heap_retire(completion_heap);
    CHECK(ores_actor_i64_heap_try_destroy(completion_heap) == 0);

    // Each independently racing completion owns one linear pin. Final CONTROL
    // destroy may race the two releases, but cannot free the heap until both
    // holders have dropped their own pin. No thread touches the heap after its
    // release call returns or after try_destroy succeeds.
    std::atomic<int> completion_ready{0};
    std::atomic<bool> completion_go{false};
    std::atomic<int> completion_destroyed{0};
    std::thread completion_a([&] {
        completion_ready.fetch_add(1);
        while (!completion_go.load(std::memory_order_acquire)) { }
        ores_actor_i64_heap_release_external(completion_heap, completion_pin_a);
    });
    std::thread completion_b([&] {
        completion_ready.fetch_add(1);
        while (!completion_go.load(std::memory_order_acquire)) { }
        ores_actor_i64_heap_release_external(completion_heap, completion_pin_b);
    });
    std::thread completion_control([&] {
        completion_ready.fetch_add(1);
        while (!completion_go.load(std::memory_order_acquire)) { }
        while (!ores_actor_i64_heap_try_destroy(completion_heap)) {
            std::this_thread::yield();
        }
        completion_destroyed.store(1, std::memory_order_release);
    });
    while (completion_ready.load(std::memory_order_acquire) != 3) { }
    completion_go.store(true, std::memory_order_release);
    completion_a.join();
    completion_b.join();
    completion_control.join();
    CHECK(completion_destroyed.load(std::memory_order_acquire) == 1);

#if defined(__unix__) || defined(__APPLE__)
    CHECK(rejects(WRONG_OWNER));
    CHECK(rejects(WRONG_EPOCH));
    CHECK(rejects(FORGED_SLOT));
    CHECK(rejects(DROP_LOANED));
    CHECK(rejects(WRITE_LOANED));
    CHECK(rejects(STALE_SLOT_REUSED));
    CHECK(rejects(FOREIGN_SLOT));
    CHECK(rejects(DOUBLE_LOAN_RELEASE));
    CHECK(rejects(DOUBLE_EXTERNAL_PIN_RELEASE));
    CHECK(rejects(REENTER));
    CHECK(rejects(CALL_AFTER_RETIRE));
    CHECK(rejects(FOREIGN_CARRIER_TURN));
    CHECK(rejects(NESTED_CARRIER_ACTOR));
#endif
    return 0;
}
