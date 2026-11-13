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
    OresActorLoan refused = {1, 2, 3, 4};
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, sa, &refused) == 0);
    REQUIRE(refused.domain == 0 && refused.nonce == 0
            && refused.index == 0 && refused.generation == 0);
    // A rejected writer never corrupts the existing shared readers.
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 10);
    ores_actor_i64_heap_release(a, shared1);
    ores_actor_i64_heap_release(a, shared2);
    OresActorLoan write_a;
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, sa, &write_a) == 1);
    REQUIRE(write_a.domain == ta.domain && write_a.nonce != 0);
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, sa, &refused) == 0);
    REQUIRE(refused.domain == 0 && refused.nonce == 0);
    // Actor B's private heap can still admit its own writer immediately.
    OresActorLoan write_b;
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(b, tb, sb, &write_b) == 1);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(a, ta, write_a) == 10);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(b, tb, write_b) == 90);
    ores_actor_i64_heap_loan_write(a, ta, write_a, 42);
    ores_actor_i64_heap_loan_write(b, tb, write_b, 91);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(a, ta, write_a) == 42);
    REQUIRE(ores_actor_i64_heap_loan_read_mut(b, tb, write_b) == 91);
    ores_actor_i64_heap_release(a, write_a);
    ores_actor_i64_heap_release(b, write_b);
    // Contention did not poison actor A or strand the writer count.
    OresActorLoan again;
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, sa, &again) == 1);
    ores_actor_i64_heap_release(a, again);
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

    // Saturate the 64-entry per-heap loan table with shared readers on sa.
    // A writer on a DIFFERENT slot must reject immediately due to quota,
    // without damaging existing read loans or poisoning the owning actor.
    OresActorSlot idle_slot = ores_actor_i64_heap_new(a, ta, 55);
    OresActorLoan readers[64];
    for (unsigned i = 0; i < 64; ++i)
        readers[i] = ores_actor_i64_heap_borrow(a, ta, sa);
    OresActorLoan at_capacity = {4, 3, 2, 1};
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, idle_slot,
                                                &at_capacity) == 0);
    REQUIRE(at_capacity.domain == 0 && at_capacity.nonce == 0);
    REQUIRE(ores_actor_i64_heap_read(a, ta, idle_slot) == 55);
    REQUIRE(ores_actor_i64_heap_read(b, tb, sb) == 91);
    for (unsigned i = 0; i < 64; ++i)
        ores_actor_i64_heap_release(a, readers[i]);
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, idle_slot,
                                                &at_capacity) == 1);
    ores_actor_i64_heap_loan_write(a, ta, at_capacity, 56);
    ores_actor_i64_heap_release(a, at_capacity);
    REQUIRE(ores_actor_i64_heap_read(a, ta, idle_slot) == 56);
    puts("HOST_ONLY_EXCLUSIVE_LOAN_QUOTA_RECOVERY_PASS");

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
    puts("HOST_ONLY_EXCLUSIVE_LOAN_CONTENTION_PASS");
    puts("HOST_ONLY_EXCLUSIVE_LOAN_POSITIVE_PASS");
}

/* Invalid guest-originated loan credentials must be reportable to the
 * caller instead of killing the native process or another independent actor.
 * This proves only the checked C ABI, not compiler branch lowering. */
static void checked_loan_status_positive(void) {
    OresActorI64Heap* a = ores_actor_i64_heap_open(301, 1, 8);
    OresActorI64Heap* sibling = ores_actor_i64_heap_open(302, 1, 8);
    OresActorTurn ta = ores_actor_i64_heap_enter(a, 301, 1);
    OresActorTurn ts = ores_actor_i64_heap_enter(sibling, 302, 1);
    OresActorSlot sa = ores_actor_i64_heap_new(a, ta, 17);
    OresActorSlot ss = ores_actor_i64_heap_new(sibling, ts, 29);
    OresActorLoan owner;
    OresActorLoan sibling_owner;
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(a, ta, sa, &owner) == 1);
    REQUIRE(ores_actor_i64_heap_try_borrow_mut(sibling, ts, ss, &sibling_owner) == 1);
    int64_t value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, owner, &value)
            == ORES_ACTOR_LOAN_OK && value == 17);
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, owner, 18)
            == ORES_ACTOR_LOAN_OK);
    value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, owner, &value)
            == ORES_ACTOR_LOAN_OK && value == 18);

    OresActorLoan wrong = owner;
    ++wrong.nonce;
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, wrong, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, wrong, &value)
            == ORES_ACTOR_LOAN_BAD_HANDLE && value == 0);
    wrong = owner;
    wrong.index = UINT64_MAX;
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, wrong, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, sibling_owner, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    REQUIRE(ores_actor_i64_heap_try_loan_write(sibling, ts, owner, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    OresActorTurn stale = {ta.domain, ta.epoch + 1};
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, stale, owner, 98)
            == ORES_ACTOR_LOAN_BAD_TURN);
    value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, stale, owner, &value)
            == ORES_ACTOR_LOAN_BAD_TURN && value == 0);
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, owner, NULL)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    REQUIRE(ores_actor_i64_heap_try_loan_write(NULL, ta, owner, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(NULL, ta, owner, &value)
            == ORES_ACTOR_LOAN_BAD_HANDLE && value == 0);

    OresActorSlot second = ores_actor_i64_heap_new(a, ta, 5);
    OresActorLoan shared = ores_actor_i64_heap_borrow(a, ta, second);
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, shared, 98)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    ores_actor_i64_heap_release(a, shared);

    // All rejected operations leave the owner and sibling heaps intact.
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, owner, &value)
            == ORES_ACTOR_LOAN_OK && value == 18);
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(
                sibling, ts, sibling_owner, &value)
            == ORES_ACTOR_LOAN_OK && value == 29);
    REQUIRE(ores_actor_i64_heap_try_loan_write(sibling, ts, sibling_owner, 30)
            == ORES_ACTOR_LOAN_OK);

    ores_actor_i64_heap_release(a, owner);
    REQUIRE(ores_actor_i64_heap_try_loan_write(a, ta, owner, 99)
            == ORES_ACTOR_LOAN_BAD_HANDLE);
    ores_actor_i64_heap_release(sibling, sibling_owner);
    REQUIRE(ores_actor_i64_heap_read(a, ta, sa) == 18);
    REQUIRE(ores_actor_i64_heap_read(sibling, ts, ss) == 30);
    ores_actor_i64_heap_leave(a, ta);
    ores_actor_i64_heap_leave(sibling, ts);
    ores_actor_i64_heap_retire(a);
    value = 999;
    REQUIRE(ores_actor_i64_heap_try_loan_read_mut(a, ta, owner, &value)
            == ORES_ACTOR_LOAN_RETIRED && value == 0);
    REQUIRE(ores_actor_i64_heap_try_destroy(a) == 1);
    ores_actor_i64_heap_retire(sibling);
    REQUIRE(ores_actor_i64_heap_try_destroy(sibling) == 1);
    puts("HOST_ONLY_EXCLUSIVE_LOAN_TYPED_FAULT_PASS");
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
    checked_loan_status_positive();
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
