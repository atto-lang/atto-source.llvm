#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <threads.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "native physical done invariant failed at line %d\n", __LINE__); \
    return 1; \
} } while (0)

typedef struct {
    OresActorI64Heap* heap;
    OresActorLoan loan;
    atomic_int* start;
    atomic_int finished;
} LoanRelease;

static int release_on_completion_thread(void* context) {
    LoanRelease* work = (LoanRelease*) context;
    while (!atomic_load_explicit(work->start, memory_order_acquire)) {
        thrd_yield();
    }
    ores_actor_i64_heap_release(work->heap, work->loan);
    atomic_store_explicit(&work->finished, 1, memory_order_release);
    return 0;
}

int main(void) {
    /*
     * Exercise the native physical-done prerequisite, NOT full LLVM source
     * suspension / cancellation semantics. A physical heap may be destroyed
     * only after CONTROL has fenced actor admission, a turn exited, and all
     * completion-thread loan releases have been joined.
     */
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        OresActorI64Heap* heap = ores_actor_i64_heap_open(
                500 + iteration, 1, 2);
        OresActorTurn turn = ores_actor_i64_heap_enter(
                heap, 500 + iteration, 1);
        OresActorSlot first = ores_actor_i64_heap_new(heap, turn, 41);
        OresActorSlot second = ores_actor_i64_heap_new(heap, turn, 42);
        OresActorLoan firstLoan = ores_actor_i64_heap_borrow(heap, turn, first);
        OresActorLoan secondLoan = ores_actor_i64_heap_borrow(heap, turn, second);

        ores_actor_i64_heap_retire(heap);
        CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);
        CHECK(ores_actor_i64_heap_loan_count(heap) == 2);
        ores_actor_i64_heap_leave(heap, turn);
        CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);

        atomic_int start = ATOMIC_VAR_INIT(0);
        LoanRelease firstRelease = {heap, firstLoan, &start, ATOMIC_VAR_INIT(0)};
        LoanRelease secondRelease = {heap, secondLoan, &start, ATOMIC_VAR_INIT(0)};
        thrd_t a, b;
        CHECK(thrd_create(&a, release_on_completion_thread, &firstRelease)
                == thrd_success);
        CHECK(thrd_create(&b, release_on_completion_thread, &secondRelease)
                == thrd_success);
        atomic_store_explicit(&start, 1, memory_order_release);

        int exit_a = -1, exit_b = -1;
        CHECK(thrd_join(a, &exit_a) == thrd_success);
        CHECK(thrd_join(b, &exit_b) == thrd_success);
        CHECK(exit_a == 0 && exit_b == 0);
        CHECK(atomic_load_explicit(&firstRelease.finished, memory_order_acquire));
        CHECK(atomic_load_explicit(&secondRelease.finished, memory_order_acquire));
        CHECK(ores_actor_i64_heap_loan_count(heap) == 0);
        CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
        /* A one-way physical-done barrier: never dereference heap again. */
    }
    puts("ORES_LLVM_NATIVE_PHYSICAL_DONE|PASS|iterations=64|loans=128");
    return 0;
}
