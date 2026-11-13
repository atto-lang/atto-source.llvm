// Host-only native C tests. Never present as compiled .atto guest evidence.
// Each forbidden operation must abort in a separate child process.
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "HOST_EXCLUSIVE_LOAN_ASSERT_FAILED line=%d expression=%s\n", __LINE__, #x); \
    exit(1); \
} } while (0)

static void positive(void) {
    OresActorI64Heap* a = ores_actor_i64_heap_open(101, 1, 8);
    OresActorI64Heap* b = ores_actor_i64_heap_open(102, 1, 8);
    OresActorTurn ta = ores_actor_i64_heap_enter(a, 101, 1);
    OresActorTurn tb = ores_actor_i64_heap_enter(b, 102, 1);
    OresActorSlot sa = ores_actor_i64_heap_new(a, ta, 10);
    OresActorSlot sb = ores_actor_i64_heap_new(b, tb, 90);
    OresActorLoan shared1 = ores_actor_i64_heap_borrow(a, ta, sa);
    OresActorLoan shared2 = ores_actor_i64_heap_borrow(a, ta, sa);
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 10);
    ores_actor_i64_heap_release(a, shared1);
    ores_actor_i64_heap_release(a, shared2);
    OresActorLoan write_a = ores_actor_i64_heap_borrow_mut(a, ta, sa);
    OresActorLoan write_b = ores_actor_i64_heap_borrow_mut(b, tb, sb);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(a, ta, write_a) == 10);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(b, tb, write_b) == 90);
    ores_actor_i64_heap_loan_write(a, ta, write_a, 42);
    ores_actor_i64_heap_loan_write(b, tb, write_b, 91);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(a, ta, write_a) == 42);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(b, tb, write_b) == 91);
    ores_actor_i64_heap_release(a, write_a);
    ores_actor_i64_heap_release(b, write_b);
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 42);
    REQUIRE(ores_actor_i64_heap_read(b, tb, sb) == 91);

    // A compiler-declared mutable field is write-loan eligible.
    ores_actor_i64_heap_field_init_mutable(a, ta.domain, ta.epoch, 2, 7);
    const OresActorSlot mutable_field = {ta.domain, 2, 1};
    OresActorLoan field_loan = ores_actor_i64_heap_borrow_mut(a, ta, mutable_field);
    ores_actor_i64_heap_loan_write(a, ta, field_loan, 99);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(a, ta, field_loan) == 99);
    ores_actor_i64_heap_release(a, field_loan);
    REQUIRE(ores_actor_i64_heap_field_read(a, ta.domain, ta.epoch, 2) == 99);

    // Shared read may span turns; direct mutation remains forbidden until release.
    OresActorLoan across_turn = ores_actor_i64_heap_borrow(a, ta, sa);
    ores_actor_i64_heap_leave(a, ta);
    ta = ores_actor_i64_heap_enter(a, 101, 1);
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 42);
    ores_actor_i64_heap_release(a, across_turn);
    OresActorLoan after_release = ores_actor_i64_heap_borrow_mut(a, ta, sa);
    ores_actor_i64_heap_loan_write(a, ta, after_release, 43);
    ores_actor_i64_heap_release(a, after_release);
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 43);
    ores_actor_i64_heap_leave(a, ta);
    ores_actor_i64_heap_leave(b, tb);
    ores_actor_i64_heap_retire(a);
    ores_actor_i64_heap_retire(b);
    REQUIRE(ores_actor_i64_heap_try_destroy(a) == 1);
    REQUIRE(ores_actor_i64_heap_try_destroy(b) == 1);
    puts("HOST_ONLY_EXCLUSIVE_LOAN_POSITIVE_PASS");
}

static void negative(unsigned case_number) {
    OresActorI64Heap* a = ores_actor_i64_heap_open(201, 1, 8);
    OresActorTurn ta = ores_actor_i64_heap_enter(a, 201, 1);
    OresActorSlot sa = ores_actor_i64_heap_new(a, ta, 15);
    OresActorLoan loan = {0, 0, 0, 0};
    switch (case_number) {
        case 1: // second writer while a shared reader exists
            (void)ores_actor_i64_heap_borrow(a, ta, sa);
            (void)ores_actor_i64_heap_borrow_mut(a, ta, sa);
            break;
        case 2: // read loan while writer exists
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            (void)ores_actor_i64_heap_borrow(a, ta, sa);
            break;
        case 3: // second writer
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            (void)ores_actor_i64_heap_borrow_mut(a, ta, sa);
            break;
        case 4: // base read aliases exclusive writer
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            (void)ores_actor_i64_heap_read(a, ta, sa);
            break;
        case 5: // base write aliases exclusive writer
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            ores_actor_i64_heap_write(a, ta, sa, 11);
            break;
        case 6: // dropping live referent with outstanding writer
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            ores_actor_i64_heap_drop(a, ta, sa);
            break;
        case 7: // double release of a nonce
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            ores_actor_i64_heap_release(a, loan);
            ores_actor_i64_heap_release(a, loan);
            break;
        case 8: // corrupted nonce
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            loan.nonce += 1;
            ores_actor_i64_heap_loan_write(a, ta, loan, 20);
            break;
        case 9: // turn generation / epoch mismatch
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            ++ta.epoch;
            ores_actor_i64_heap_loan_write(a, ta, loan, 20);
            break;
        case 10: // mutable loan cannot cross await/turn leave
            loan = ores_actor_i64_heap_borrow_mut(a, ta, sa);
            ores_actor_i64_heap_leave(a, ta);
            break;
        case 11: // foreign actor loan must not be usable in this heap
        {
            OresActorI64Heap* b = ores_actor_i64_heap_open(202, 1, 8);
            OresActorTurn tb = ores_actor_i64_heap_enter(b, 202, 1);
            OresActorSlot sb = ores_actor_i64_heap_new(b, tb, 22);
            loan = ores_actor_i64_heap_borrow_mut(b, tb, sb);
            ores_actor_i64_heap_loan_write(a, ta, loan, 30);
            break;
        }
        case 12: // immutable field may not have a write loan
            ores_actor_i64_heap_field_init(a, ta.domain, ta.epoch, 3, 1);
            (void)ores_actor_i64_heap_borrow_mut(a, ta,
                                                 (OresActorSlot){ta.domain, 3, 1});
            break;
        case 13: // direct field write cannot alias exclusive writer
            ores_actor_i64_heap_field_init_mutable(a, ta.domain, ta.epoch, 3, 1);
            loan = ores_actor_i64_heap_borrow_mut(a, ta,
                                                  (OresActorSlot){ta.domain, 3, 1});
            ores_actor_i64_heap_field_write(a, ta.domain, ta.epoch, 3, 2);
            break;
        case 14: // direct field read cannot alias exclusive writer
            ores_actor_i64_heap_field_init_mutable(a, ta.domain, ta.epoch, 3, 1);
            loan = ores_actor_i64_heap_borrow_mut(a, ta,
                                                  (OresActorSlot){ta.domain, 3, 1});
            (void)ores_actor_i64_heap_field_read(a, ta.domain, ta.epoch, 3);
            break;
        case 15: // loan to stale dropped slot
            ores_actor_i64_heap_drop(a, ta, sa);
            (void)ores_actor_i64_heap_borrow_mut(a, ta, sa);
            break;
        case 16: // release a shared loan as an exclusive writer token
            loan = ores_actor_i64_heap_borrow(a, ta, sa);
            ores_actor_i64_heap_loan_write(a, ta, loan, 20);
            break;
        default: _exit(99);
    }
    _exit(0); // unexpected successful admission
}

int main(void) {
    const struct rlimit core = {0, 0};
    REQUIRE(setrlimit(RLIMIT_CORE, &core) == 0);
    positive();
    for (unsigned test = 1; test <= 16; ++test) {
        pid_t child = fork();
        REQUIRE(child >= 0);
        if (child == 0) negative(test);
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        printf("HOST_ONLY_EXCLUSIVE_LOAN_NEGATIVE_PASS case=%u\n", test);
    }
    puts("HOST_ONLY_EXCLUSIVE_LOAN_SUITE_PASS positives=1 abort_negatives=16");
    return 0;
}
