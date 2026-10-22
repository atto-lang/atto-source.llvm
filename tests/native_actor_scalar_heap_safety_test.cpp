#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "scalar actor heap failure:%d\n", __LINE__); return 1; } } while (0)

#if defined(ORES_ARENA_TEST_ALLOCATIONS)
static bool forbid_allocation;
extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
void* __wrap_malloc(size_t n) { if (forbid_allocation) abort(); return __real_malloc(n); }
void* __wrap_calloc(size_t n, size_t s) { if (forbid_allocation) abort(); return __real_calloc(n, s); }
void* __wrap_realloc(void* p, size_t n) { if (forbid_allocation) abort(); return __real_realloc(p, n); }
void __wrap_free(void* p) { if (forbid_allocation) abort(); __real_free(p); }
}
#endif
enum Misuse { STOLEN_READ, STOLEN_WRITE, STOLEN_LEAVE, STOLEN_FIELD_READ,
    STOLEN_FIELD_WRITE, NESTED_HEAP, READ_WHILE_MUT, WRITE_WHILE_MUT,
    DROP_WHILE_MUT, SHARED_WHILE_MUT, MUT_WHILE_MUT, MUT_WHILE_SHARED,
    WRITE_SHARED_LOAN, RELEASED_LOAN, REPLAY_AFTER_REUSE, BAD_LOAN_INDEX,
    BAD_LOAN_GENERATION, BAD_LOAN_DOMAIN, BAD_LOAN_NONCE, STOLEN_LOAN,
    IMMUTABLE_FIELD_MUT, FIELD_READ_WHILE_MUT, FIELD_WRITE_WHILE_MUT,
    LATE_FIELD_INIT, REINIT_FIELD, RETIRED_READ, RETIRED_LOAN, QUOTA,
    FOREIGN_SLOT, LAST_MISUSE = FOREIGN_SLOT };
static void invalid(Misuse which) {
    auto* heap = ores_actor_i64_heap_open(17, 3, 5);
    auto turn = ores_actor_i64_heap_enter(heap, 17, 3);
    ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 21);
    ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 3, 0);
    auto slot = ores_actor_i64_heap_new(heap, turn, 42);
    if (which <= STOLEN_FIELD_WRITE) {
        std::thread thief([&] {
            if (which == STOLEN_READ) (void)ores_actor_i64_heap_read(heap, turn, slot);
            else if (which == STOLEN_WRITE) ores_actor_i64_heap_write(heap, turn, slot, 1);
            else if (which == STOLEN_LEAVE) ores_actor_i64_heap_leave(heap, turn);
            else if (which == STOLEN_FIELD_READ) (void)ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 1);
            else ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, 3, 1);
        });
        thief.join();
    } else if (which == NESTED_HEAP) {
        auto* other = ores_actor_i64_heap_open(18, 3, 1);
        (void)ores_actor_i64_heap_enter(other, 18, 3);
    } else if (which == LATE_FIELD_INIT) {
        ores_actor_i64_heap_leave(heap, turn);
        turn = ores_actor_i64_heap_enter(heap, 17, 3);
        ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 4, 9);
    } else if (which == REINIT_FIELD) {
        ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 9);
    } else if (which == IMMUTABLE_FIELD_MUT) {
        OresActorSlot immutable = {turn.domain, 1, 1};
        (void)ores_actor_i64_heap_borrow_mut(heap, turn, immutable);
    } else if (which == QUOTA) {
        for (int i = 0; i < 65; ++i) (void)ores_actor_i64_heap_borrow(heap, turn, slot);
    } else if (which == FOREIGN_SLOT) {
        ores_actor_i64_heap_leave(heap, turn);
        auto* other = ores_actor_i64_heap_open(17, 3, 1);
        auto next = ores_actor_i64_heap_enter(other, 17, 3);
        (void)ores_actor_i64_heap_new(other, next, 8);
        (void)ores_actor_i64_heap_read(other, next, slot);
    } else {
        if (which == FIELD_READ_WHILE_MUT || which == FIELD_WRITE_WHILE_MUT)
            slot = {turn.domain, 3, 1};
        auto loan = which == WRITE_SHARED_LOAN || which == MUT_WHILE_SHARED
            ? ores_actor_i64_heap_borrow(heap, turn, slot)
            : ores_actor_i64_heap_borrow_mut(heap, turn, slot);
        if (which == READ_WHILE_MUT) (void)ores_actor_i64_heap_read(heap, turn, slot);
        else if (which == WRITE_WHILE_MUT) ores_actor_i64_heap_write(heap, turn, slot, 1);
        else if (which == DROP_WHILE_MUT) ores_actor_i64_heap_drop(heap, turn, slot);
        else if (which == SHARED_WHILE_MUT) (void)ores_actor_i64_heap_borrow(heap, turn, slot);
        else if (which == MUT_WHILE_MUT || which == MUT_WHILE_SHARED) (void)ores_actor_i64_heap_borrow_mut(heap, turn, slot);
        else if (which == WRITE_SHARED_LOAN) ores_actor_i64_heap_loan_write(heap, turn, loan, 1);
        else if (which == FIELD_READ_WHILE_MUT) (void)ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 3);
        else if (which == FIELD_WRITE_WHILE_MUT) ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, 3, 1);
        else if (which == STOLEN_LOAN) {
            std::thread thief([&] { (void)ores_actor_i64_heap_loan_read(heap, turn, loan); });
            thief.join();
        } else if (which == RETIRED_READ || which == RETIRED_LOAN) {
            ores_actor_i64_heap_retire(heap);
            if (which == RETIRED_READ) (void)ores_actor_i64_heap_read(heap, turn, slot);
            else ores_actor_i64_heap_loan_write(heap, turn, loan, 1);
        } else {
            if (which == RELEASED_LOAN || which == REPLAY_AFTER_REUSE) {
                ores_actor_i64_heap_release(heap, loan);
                if (which == REPLAY_AFTER_REUSE) (void)ores_actor_i64_heap_borrow_mut(heap, turn, slot);
            } else if (which == BAD_LOAN_INDEX) ++loan.index;
            else if (which == BAD_LOAN_GENERATION) ++loan.generation;
            else if (which == BAD_LOAN_DOMAIN) ++loan.domain;
            else if (which == BAD_LOAN_NONCE) ++loan.nonce;
            (void)ores_actor_i64_heap_loan_read(heap, turn, loan);
        }
    }
}
#if defined(__unix__) || defined(__APPLE__)
static bool rejects(Misuse which) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (!pid) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        invalid(which); _exit(91);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif
int main() {
    // Actor slot zero is a legitimate scheduler identity. It must not be
    // confused with a missing heap domain or incarnation authority.
    auto* zero = ores_actor_i64_heap_open(0, 9, 2);
    auto zero_turn = ores_actor_i64_heap_enter(zero, 0, 9);
    auto zero_slot = ores_actor_i64_heap_new(zero, zero_turn, 90210);
    CHECK(ores_actor_i64_heap_read(zero, zero_turn, zero_slot) == 90210);
    ores_actor_i64_heap_leave(zero, zero_turn);
    ores_actor_i64_heap_retire(zero);
    CHECK(ores_actor_i64_heap_try_destroy(zero) == 1);
    auto* heap = ores_actor_i64_heap_open(17, 3, 4096);
#if defined(ORES_ARENA_TEST_ALLOCATIONS)
    forbid_allocation = true;
#endif
    auto turn = ores_actor_i64_heap_enter(heap, 17, 3);
    // Reserve fixed fields out of free-list order; dynamic slots must never
    // collide with either field, even after thousands of reuse cycles.
    ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 4095, 0);
    ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 21);
    OresActorSlot slots[4094];
    for (int i = 0; i < 4094; ++i) {
        slots[i] = ores_actor_i64_heap_new(heap, turn, i);
        CHECK(slots[i].index != 1 && slots[i].index != 4095);
    }
    for (int i = 0; i < 4094; ++i) {
        CHECK(ores_actor_i64_heap_read(heap, turn, slots[i]) == i);
        ores_actor_i64_heap_drop(heap, turn, slots[i]);
    }
    auto slot = ores_actor_i64_heap_new(heap, turn, 42);
    for (int i = 0; i < 16384; ++i) {
        auto writer = ores_actor_i64_heap_borrow_mut(heap, turn, slot);
        ores_actor_i64_heap_loan_write(heap, turn, writer, i);
        CHECK(ores_actor_i64_heap_loan_read(heap, turn, writer) == i);
        ores_actor_i64_heap_release(heap, writer);
        ores_actor_i64_heap_drop(heap, turn, slot);
        slot = ores_actor_i64_heap_new(heap, turn, 42);
    }
    OresActorSlot field = {turn.domain, 4095, 1};
    auto writer = ores_actor_i64_heap_borrow_mut(heap, turn, field);
    ores_actor_i64_heap_loan_write(heap, turn, writer, 43);
    ores_actor_i64_heap_release(heap, writer);
    CHECK(ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 4095) == 43);
    OresActorLoan readers[64];
    for (int i = 0; i < 64; ++i) readers[i] = ores_actor_i64_heap_borrow(heap, turn, slot);
    for (const auto& reader : readers) {
        CHECK(ores_actor_i64_heap_loan_read(heap, turn, reader) == 42);
        ores_actor_i64_heap_release(heap, reader);
    }
    writer = ores_actor_i64_heap_borrow_mut(heap, turn, slot);
    ores_actor_i64_heap_leave(heap, turn);
#if defined(ORES_ARENA_TEST_ALLOCATIONS)
    forbid_allocation = false;
#endif
    std::thread migrate([&] {
        auto next = ores_actor_i64_heap_enter(heap, 17, 3);
        ores_actor_i64_heap_loan_write(heap, next, writer, 44);
        ores_actor_i64_heap_leave(heap, next);
    });
    migrate.join();
    turn = ores_actor_i64_heap_enter(heap, 17, 3);
    CHECK(ores_actor_i64_heap_loan_read(heap, turn, writer) == 44);
    ores_actor_i64_heap_retire(heap);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);
    ores_actor_i64_heap_leave(heap, turn); // retirement must allow unwind
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 0);
    std::thread release([&] { ores_actor_i64_heap_release(heap, writer); });
    release.join();
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i <= LAST_MISUSE; ++i) {
        if (!rejects(static_cast<Misuse>(i))) {
            std::fprintf(stderr, "scalar heap admitted misuse %d\n", i); return 1;
        }
    }
    std::printf("scalar actor heap: %d misuse rejections; full-capacity/reuse/migration passed\n", LAST_MISUSE + 1);
#endif
    return 0;
}
