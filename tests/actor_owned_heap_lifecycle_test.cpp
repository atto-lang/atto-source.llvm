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

#if !defined(ORES_HEAP_NO_LINK_WRAP)
/* Linker --wrap intercepts calls from production ActorOwnedI64Heap.c,
 * not its C++ test host's unrelated allocator traffic. Check actual bytes
 * BEFORE real free at O0/O2, never after the allocation lifetime ends.
 * This is scalar-heap direct linkage, NOT process-wide GC certification. */
struct TrackedHeapAllocation { void* p; size_t bytes; };
static TrackedHeapAllocation tracked_heap[32];
static size_t tracked_heap_count;
static size_t scrubbed_physical_frees;
extern "C" void* __real_calloc(size_t count, size_t size);
extern "C" void __real_free(void*);
extern "C" void* __wrap_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) abort();
    void* p = __real_calloc(count, size);
    if (p) {
        if (tracked_heap_count == 32) abort();
        tracked_heap[tracked_heap_count++] = TrackedHeapAllocation{p, count * size};
    }
    return p;
}
extern "C" void __wrap_free(void* p) {
    if (!p) return;
    size_t i = 0;
    while (i < tracked_heap_count && tracked_heap[i].p != p) ++i;
    if (i == tracked_heap_count) abort();
    const volatile unsigned char* bytes =
        static_cast<const volatile unsigned char*>(p);
    for (size_t j = 0; j < tracked_heap[i].bytes; ++j) {
        if (bytes[j] != 0) {
            fputs("ACTOR_OWNED_I64_HEAP_UNSCRUBBED_PRE_FREE\n", stderr);
            abort();
        }
    }
    tracked_heap[i] = tracked_heap[--tracked_heap_count];
    ++scrubbed_physical_frees;
    __real_free(p);
}
#endif /* !ORES_HEAP_NO_LINK_WRAP */

enum Misuse { WRONG_OWNER, WRONG_EPOCH, FORGED_SLOT, DROP_LOANED, WRITE_LOANED, STALE_SLOT_REUSED, FOREIGN_SLOT, DOUBLE_LOAN_RELEASE, REENTER, CALL_AFTER_RETIRE, CROSS_CARRIER_TOKEN, NESTED_DIFFERENT_HEAP };
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
    } else if (which == CROSS_CARRIER_TOKEN) {
        /* Logical domain/epoch match, but this token was admitted on the
         * forked child's main carrier, not on the child native thread. */
        std::thread thief([&] {
            (void) ores_actor_i64_heap_read(heap, turn, slot);
        });
        thief.join();
    } else if (which == NESTED_DIFFERENT_HEAP) {
        /* Two actor turns cannot execute simultaneously on one carrier,
         * even if they have different domain IDs. */
        OresActorI64Heap* sibling = ores_actor_i64_heap_open(30, 8, 2);
        (void) ores_actor_i64_heap_enter(sibling, 30, 8);
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

static int test_long_lived_source_actor_heap_churn(void) {
    /* 100,000 transients on ONE heap rather than 100,000 short-lived
     * processes. A persistent compiler-declared field must survive all
     * reuses. Each temporary allocation is explicitly dropped. */
    const uint64_t owner = 3301u, incarnation = 11u;
    OresActorI64Heap* heap = ores_actor_i64_heap_open(owner, incarnation, 2u);
    CHECK(heap != nullptr);
    OresActorTurn initial = ores_actor_i64_heap_enter(heap, owner, incarnation);
    ores_actor_i64_heap_field_init_mutable(
        heap, initial.domain, initial.epoch, 0u, 41);
    ores_actor_i64_heap_leave(heap, initial);
    uint64_t last_generation = 0;
    for (int turn_index = 0; turn_index < 1000; ++turn_index) {
        OresActorTurn turn = ores_actor_i64_heap_enter(heap, owner, incarnation);
        CHECK(ores_actor_i64_heap_field_read(
                  heap, turn.domain, turn.epoch, 0u) == 41 + turn_index);
        for (int j = 0; j < 100; ++j) {
            const int64_t value = (int64_t)turn_index * 100 + j;
            OresActorSlot temporary = ores_actor_i64_heap_new(heap, turn, value);
            CHECK(temporary.domain == turn.domain && temporary.index == 1u);
            CHECK(temporary.generation > last_generation);
            last_generation = temporary.generation;
            CHECK(ores_actor_i64_heap_read(heap, turn, temporary) == value);
            ores_actor_i64_heap_write(heap, turn, temporary, value + 1);
            CHECK(ores_actor_i64_heap_read(heap, turn, temporary) == value + 1);
            ores_actor_i64_heap_drop(heap, turn, temporary);
        }
        ores_actor_i64_heap_field_write(
            heap, turn.domain, turn.epoch, 0u, 42 + turn_index);
        ores_actor_i64_heap_leave(heap, turn);
    }
    OresActorTurn final_turn = ores_actor_i64_heap_enter(heap, owner, incarnation);
    CHECK(ores_actor_i64_heap_field_read(
              heap, final_turn.domain, final_turn.epoch, 0u) == 1041);
    /* Simulate a suspended read borrow. Its lease outlives the native
     * turn and can be released from a different trusted host carrier. */
    OresActorSlot field = {final_turn.domain, 0u, 1u};
    OresActorLoan suspended = ores_actor_i64_heap_borrow(heap, final_turn, field);
    /* Supervisor cancellation may retire a heap while its final actor turn
     * is still unwinding. Neither an active turn nor its cross-turn loan
     * permits physical release. A retired turn may still leave. */
    ores_actor_i64_heap_retire(heap);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);
    ores_actor_i64_heap_leave(heap, final_turn);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);
    std::thread releaser([&] { ores_actor_i64_heap_release(heap, suspended); });
    releaser.join();
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
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
    CHECK(rejects(CROSS_CARRIER_TOKEN));
    CHECK(rejects(NESTED_DIFFERENT_HEAP));
    CHECK(test_long_lived_source_actor_heap_churn() == 0);
#if !defined(ORES_HEAP_NO_LINK_WRAP) /* Apple ld has no --wrap: no pre-free scrub evidence there */
    CHECK(tracked_heap_count == 0);
    CHECK(scrubbed_physical_frees == 2);
#endif
    puts("SOURCE_ACTOR_HEAP_HOST_LIFECYCLE_OK|churn=100000|churn_turns=1000|negative=12|scrubbed_heap_frees=2|atto_guest=NO");
#endif
    return 0;
}
