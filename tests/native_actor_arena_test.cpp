#include "oreslang/runtime/ActorArena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "native actor arena failed:%d\n", __LINE__); return 1; } } while (0)
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
enum Misuse { STALE_TURN, FOREIGN_OWNER, OVERLAP, SCRATCH_AFTER_LEAVE, WRITE_LOANED, DROP_LOANED, DOUBLE_RELEASE, OUT_OF_BOUNDS, SCRATCH_OVERFLOW, BAD_ALIGNMENT, OVERSIZED_ACTOR_BUDGET, FOREIGN_CARRIER_TURN, NESTED_ACTOR_TURN,
    MUT_AFTER_SHARED, SHARED_AFTER_MUT, MUT_AFTER_MUT, OWNER_READ_MUT, OWNER_WRITE_MUT,
    SHARED_LOAN_WRITE, RELEASED_LOAN_READ, FORGED_LOAN_INDEX, FORGED_LOAN_GENERATION,
    FORGED_LOAN_NONCE, FOREIGN_LOAN_DOMAIN, LOAN_READ_BOUNDS, LOAN_WRITE_BOUNDS,
    LOAN_READ_NULL, LOAN_WRITE_NULL, STALE_LOAN_TURN, RETIRED_LOAN_READ,
    RETIRED_LOAN_WRITE, FOREIGN_CARRIER_LOAN, LOAN_QUOTA, FOREIGN_ARENA_HANDLE,
    RELEASED_LOAN_REUSE, LAST_MISUSE = RELEASED_LOAN_REUSE };
typedef struct { OresActorArena* arena; OresArenaTurn turn; OresArenaHandle handle; } WrongCarrier;
static int different_carrier_with_stolen_turn(void* opaque) {
    WrongCarrier* x = (WrongCarrier*) opaque;
    int value = 0;
    ores_actor_arena_read(x->arena, x->turn, x->handle, 0, &value, sizeof(value));
    return 0;
}
static void invalid(enum Misuse m) {
    OresActorArena* a = ores_actor_arena_open(23, 2, 2, 64, 128);
    OresArenaTurn t = ores_actor_arena_enter(a, 23, 2);
    OresArenaHandle h = ores_actor_arena_alloc(a, t, 16);
    OresArenaScratch s = ores_actor_arena_scratch_alloc(a, t, 16, 8);
    if (m == STALE_TURN) {
        ores_actor_arena_leave(a, t);
        (void) ores_actor_arena_enter(a, 23, 2);
        int v; ores_actor_arena_read(a, t, h, 0, &v, sizeof(v));
    } else if (m == FOREIGN_OWNER) {
        ores_actor_arena_leave(a, t);
        (void) ores_actor_arena_enter(a, 24, 2);
    } else if (m == OVERLAP) {
        (void) ores_actor_arena_enter(a, 23, 2);
    } else if (m == SCRATCH_AFTER_LEAVE) {
        ores_actor_arena_leave(a, t);
        OresArenaTurn next = ores_actor_arena_enter(a, 23, 2);
        int v; ores_actor_arena_scratch_read(a, next, s, 0, &v, sizeof(v));
    } else if (m == WRITE_LOANED) {
        (void) ores_actor_arena_borrow(a, t, h);
        int v = 1; ores_actor_arena_write(a, t, h, 0, &v, sizeof(v));
    } else if (m == DROP_LOANED) {
        (void) ores_actor_arena_borrow(a, t, h);
        ores_actor_arena_drop(a, t, h);
    } else if (m == DOUBLE_RELEASE) {
        OresArenaLoan l = ores_actor_arena_borrow(a, t, h);
        ores_actor_arena_release(a, l);
        ores_actor_arena_release(a, l);
    } else if (m == OUT_OF_BOUNDS) {
        int v; ores_actor_arena_read(a, t, h, 15, &v, sizeof(v));
    } else if (m == SCRATCH_OVERFLOW) {
        (void) ores_actor_arena_scratch_alloc(a, t, 200, 8);
    } else if (m == BAD_ALIGNMENT) {
        (void) ores_actor_arena_scratch_alloc(a, t, 8, 3);
    } else if (m == OVERSIZED_ACTOR_BUDGET) {
        (void) ores_actor_arena_open(24, 4, 4096, 1024 * 1024, 128);
    } else if (m == FOREIGN_CARRIER_TURN) {
        WrongCarrier wrong = {a, t, h};
        std::thread other([&wrong] { different_carrier_with_stolen_turn(&wrong); });
        other.join();
    } else if (m == NESTED_ACTOR_TURN) {
        OresActorArena* other = ores_actor_arena_open(33, 2, 1, 16, 16);
        (void) ores_actor_arena_enter(other, 33, 2);
    } else if (m == MUT_AFTER_SHARED) {
        (void) ores_actor_arena_borrow(a, t, h);
        (void) ores_actor_arena_borrow_mut(a, t, h);
    } else if (m == LOAN_QUOTA) {
        for (int i = 0; i < 65; ++i) (void) ores_actor_arena_borrow(a, t, h);
    } else if (m == FOREIGN_ARENA_HANDLE) {
        OresActorArena* other = ores_actor_arena_open(23, 2, 2, 64, 128);
        ores_actor_arena_leave(a, t);
        OresArenaTurn next = ores_actor_arena_enter(other, 23, 2);
        (void) ores_actor_arena_alloc(other, next, 16);
        int v = 1;
        ores_actor_arena_write(other, next, h, 0, &v, sizeof(v));
    } else {
        OresArenaLoan loan = m == SHARED_LOAN_WRITE
            ? ores_actor_arena_borrow(a, t, h) : ores_actor_arena_borrow_mut(a, t, h);
        int v = 1;
        if (m == SHARED_AFTER_MUT) (void) ores_actor_arena_borrow(a, t, h);
        else if (m == MUT_AFTER_MUT) (void) ores_actor_arena_borrow_mut(a, t, h);
        else if (m == OWNER_READ_MUT) ores_actor_arena_read(a, t, h, 0, &v, sizeof(v));
        else if (m == OWNER_WRITE_MUT) ores_actor_arena_write(a, t, h, 0, &v, sizeof(v));
        else if (m == SHARED_LOAN_WRITE) ores_actor_arena_loan_write(a, t, loan, 0, &v, sizeof(v));
        else if (m == RELEASED_LOAN_REUSE) {
            ores_actor_arena_release(a, loan);
            (void) ores_actor_arena_borrow_mut(a, t, h);
            ores_actor_arena_loan_write(a, t, loan, 0, &v, sizeof(v));
        }
        else if (m == RELEASED_LOAN_READ) {
            ores_actor_arena_release(a, loan);
            ores_actor_arena_loan_read(a, t, loan, 0, &v, sizeof(v));
        } else if (m == LOAN_READ_BOUNDS) ores_actor_arena_loan_read(a, t, loan, 15, &v, sizeof(v));
        else if (m == LOAN_WRITE_BOUNDS) ores_actor_arena_loan_write(a, t, loan, 15, &v, sizeof(v));
        else if (m == LOAN_READ_NULL) ores_actor_arena_loan_read(a, t, loan, 0, NULL, 1);
        else if (m == LOAN_WRITE_NULL) ores_actor_arena_loan_write(a, t, loan, 0, NULL, 1);
        else if (m == STALE_LOAN_TURN) {
            ores_actor_arena_leave(a, t);
            (void) ores_actor_arena_enter(a, 23, 2);
            ores_actor_arena_loan_read(a, t, loan, 0, &v, sizeof(v));
        } else if (m == RETIRED_LOAN_READ || m == RETIRED_LOAN_WRITE) {
            ores_actor_arena_retire(a);
            if (m == RETIRED_LOAN_READ) ores_actor_arena_loan_read(a, t, loan, 0, &v, sizeof(v));
            else ores_actor_arena_loan_write(a, t, loan, 0, &v, sizeof(v));
        } else if (m == FOREIGN_CARRIER_LOAN) {
            std::thread other([&] { ores_actor_arena_loan_read(a, t, loan, 0, &v, sizeof(v)); });
            other.join();
        } else {
            if (m == FORGED_LOAN_INDEX) ++loan.index;
            else if (m == FORGED_LOAN_GENERATION) ++loan.generation;
            else if (m == FORGED_LOAN_NONCE) ++loan.nonce;
            else if (m == FOREIGN_LOAN_DOMAIN) ++loan.domain;
            ores_actor_arena_loan_read(a, t, loan, 0, &v, sizeof(v));
        }
    }
}
#if defined(__unix__) || defined(__APPLE__)
static int rejects(enum Misuse m) {
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        struct rlimit lim = {0,0};
        (void) setrlimit(RLIMIT_CORE, &lim);
        invalid(m); _exit(91);
    }
    int st = 0;
    return waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
#endif
typedef struct {OresActorArena* a; OresArenaHandle h; int failed;} Carrier;
static int other_carrier(void* p) {
    Carrier* v = (Carrier*) p;
    OresArenaTurn t = ores_actor_arena_enter(v->a, 23, 2);
    int value = 0;
    ores_actor_arena_read(v->a, t, v->h, 0, &value, sizeof(value));
    if (value != 42) v->failed = 1;
    OresArenaScratch s = ores_actor_arena_scratch_alloc(v->a, t, sizeof(value), 4);
    ores_actor_arena_scratch_read(v->a, t, s, 0, &value, sizeof(value));
    if (value != 0) v->failed = 1;
    ores_actor_arena_leave(v->a, t);
    return 0;
}
int main(void) {
    OresActorArena* a = ores_actor_arena_open(23, 2, 2, 64, 128);
#if defined(ORES_ARENA_TEST_ALLOCATIONS)
    forbid_allocation = true;
#endif
    OresArenaTurn first = ores_actor_arena_enter(a, 23, 2);
    OresArenaHandle h = ores_actor_arena_alloc(a, first, 16);
    /* Repeated allocations reuse a free slot without malloc or scan, and
     * monotonic generations reject stale handles after reuse. */
    uint64_t last_generation = 0;
    for (int i = 0; i < 8192; ++i) {
        OresArenaHandle temp = ores_actor_arena_alloc(a, first, 8);
        CHECK(temp.index == 1 && temp.generation > last_generation);
        last_generation = temp.generation;
        ores_actor_arena_drop(a, first, temp);
    }
    int value = 42;
    ores_actor_arena_write(a, first, h, 0, &value, sizeof(value));
    /* Repeated exclusive borrows are allocation-free and suspend owner I/O.
     * A loan's nonce is never recycled when the fixed loan table is reused. */
    uint64_t previous_nonce = 0;
    for (int i = 0; i < 8192; ++i) {
        OresArenaLoan writer = ores_actor_arena_borrow_mut(a, first, h);
        CHECK(writer.nonce > previous_nonce);
        previous_nonce = writer.nonce;
        ores_actor_arena_loan_write(a, first, writer, 0, &value, sizeof(value));
        int observed = 0;
        ores_actor_arena_loan_read(a, first, writer, 0, &observed, sizeof(observed));
        CHECK(observed == 42);
        ores_actor_arena_release(a, writer);
    }
    /* Shared readers coexist, while a disjoint slot may have a writer. */
    OresArenaLoan readers[64];
    for (int i = 0; i < 64; ++i) readers[i] = ores_actor_arena_borrow(a, first, h);
    for (int i = 0; i < 64; ++i) {
        int observed = 0;
        ores_actor_arena_loan_read(a, first, readers[i], 0, &observed, sizeof(observed));
        CHECK(observed == 42);
        ores_actor_arena_release(a, readers[i]);
    }
    OresArenaHandle second = ores_actor_arena_alloc(a, first, sizeof(value));
    OresArenaLoan reader = ores_actor_arena_borrow(a, first, h);
    OresArenaLoan disjoint = ores_actor_arena_borrow_mut(a, first, second);
    ores_actor_arena_loan_write(a, first, disjoint, 0, &value, sizeof(value));
    ores_actor_arena_release(a, disjoint);
    ores_actor_arena_release(a, reader);
    ores_actor_arena_drop(a, first, second);
    OresArenaScratch s = ores_actor_arena_scratch_alloc(a, first, 8, 8);
    ores_actor_arena_scratch_write(a, first, s, 0, &value, sizeof(value));
    value = 0; ores_actor_arena_scratch_read(a, first, s, 0, &value, sizeof(value));
    CHECK(value == 42);
    OresArenaLoan hold = ores_actor_arena_borrow(a, first, h);
    ores_actor_arena_leave(a, first);
#if defined(ORES_ARENA_TEST_ALLOCATIONS)
    forbid_allocation = false;
#endif
    /* Durable loans survive turns; scratch is zeroized and recycled. */
    Carrier car = {a, h, 0};
    int outcome = -1;
    std::thread carrier([&] { outcome = other_carrier(&car); });
    carrier.join();
    CHECK(outcome == 0 && !car.failed);
    OresArenaTurn last = ores_actor_arena_enter(a, 23, 2);
    value = 0; ores_actor_arena_read(a, last, h, 0, &value, sizeof(value));
    CHECK(value == 42);
    ores_actor_arena_leave(a, last);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);
    ores_actor_arena_release(a, hold);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    /* Equal numeric owner/incarnation values cannot alias separate arenas. */
    OresActorArena* isolated = ores_actor_arena_open(23, 5, 1, 32, 64);
    a = ores_actor_arena_open(23, 5, 1, 32, 64);
    first = ores_actor_arena_enter(a, 23, 5);
    h = ores_actor_arena_alloc(a, first, sizeof(value));
    value = 17;
    ores_actor_arena_write(a, first, h, 0, &value, sizeof(value));
    ores_actor_arena_leave(a, first);
    OresArenaTurn isolated_turn = ores_actor_arena_enter(isolated, 23, 5);
    OresArenaHandle isolated_handle = ores_actor_arena_alloc(isolated, isolated_turn, sizeof(value));
    CHECK(isolated_handle.domain != h.domain);
    value = -1;
    ores_actor_arena_read(isolated, isolated_turn, isolated_handle, 0, &value, sizeof(value));
    CHECK(value == 0);
    value = 99;
    ores_actor_arena_write(isolated, isolated_turn, isolated_handle, 0, &value, sizeof(value));
    ores_actor_arena_leave(isolated, isolated_turn);
    first = ores_actor_arena_enter(a, 23, 5);
    ores_actor_arena_read(a, first, h, 0, &value, sizeof(value));
    CHECK(value == 17);
    ores_actor_arena_leave(a, first);
    ores_actor_arena_retire(a);
    ores_actor_arena_retire(isolated);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    CHECK(ores_actor_arena_try_destroy(isolated) == 1);
    value = 42;
    /* Exclusive loans survive carrier migration, but every access needs the
     * newly admitted actor turn. External release may unblock retirement. */
    a = ores_actor_arena_open(23, 4, 1, 32, 64);
    first = ores_actor_arena_enter(a, 23, 4);
    h = ores_actor_arena_alloc(a, first, sizeof(value));
    OresArenaLoan writer = ores_actor_arena_borrow_mut(a, first, h);
    ores_actor_arena_leave(a, first);
    std::thread migrate([&] {
        OresArenaTurn next = ores_actor_arena_enter(a, 23, 4);
        ores_actor_arena_loan_write(a, next, writer, 0, &value, sizeof(value));
        ores_actor_arena_leave(a, next);
    });
    migrate.join();
    last = ores_actor_arena_enter(a, 23, 4);
    int migrated = 0;
    ores_actor_arena_loan_read(a, last, writer, 0, &migrated, sizeof(migrated));
    CHECK(migrated == 42);
    ores_actor_arena_leave(a, last);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);
    std::thread release([&] { ores_actor_arena_release(a, writer); });
    release.join();
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    /* Supervisor cancellation may retire a heap during an active turn. */
    a = ores_actor_arena_open(23, 3, 1, 32, 64);
    OresArenaTurn unwinding = ores_actor_arena_enter(a, 23, 3);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);
    ores_actor_arena_leave(a, unwinding);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
#if defined(__unix__) || defined(__APPLE__)
    for (int n = STALE_TURN; n <= LAST_MISUSE; ++n) {
        if (!rejects((enum Misuse) n)) {
            fprintf(stderr, "native actor arena admitted misuse %d\n", n);
            return 1;
        }
    }
    printf("native actor arena: %d misuse rejections; slot/loan reuse, isolation and migration passed\n", LAST_MISUSE + 1);
#else
    printf("native actor arena: positive lifecycle tests passed; POSIX misuse tests unavailable\n");
#endif
    return 0;
}
