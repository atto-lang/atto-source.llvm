#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <atomic>
#include <array>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "actor heap check failed at %d\n", __LINE__); return 1; } } while (0)

enum Misuse { WRONG_OWNER, WRONG_EPOCH, FORGED_SLOT, DROP_LOANED, WRITE_LOANED, STALE_SLOT_REUSED, FOREIGN_SLOT, DOUBLE_LOAN_RELEASE, REENTER, CALL_AFTER_RETIRE, FOREIGN_CARRIER_TURN, NESTED_CARRIER_ACTOR, FORGED_EXTERNAL_LEASE, FORGED_EXTERNAL_DOMAIN, EXTERNAL_AFTER_RETIRE };
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
    } else if (which == NESTED_CARRIER_ACTOR) {
        OresActorI64Heap* other = ores_actor_i64_heap_open(30, 8, 1);
        (void) ores_actor_i64_heap_enter(other, 30, 8);
    } else if (which == FORGED_EXTERNAL_LEASE) {
        OresActorExternalLease lease = ores_actor_i64_heap_external_acquire(heap, turn);
        ++lease.generation;
        ores_actor_i64_heap_external_release(heap, &lease);
    } else if (which == FORGED_EXTERNAL_DOMAIN) {
        OresActorExternalLease lease = ores_actor_i64_heap_external_acquire(heap, turn);
        ++lease.domain;
        ores_actor_i64_heap_external_release(heap, &lease);
    } else if (which == EXTERNAL_AFTER_RETIRE) {
        ores_actor_i64_heap_retire(heap);
        (void) ores_actor_i64_heap_external_acquire(heap, turn);
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

/* Adversarial scheduler admission: contenders never spin inside the heap
 * on an expected busy state; they yield and resubmit. This native serial
 * backend implements max executing turns=1, not the future opt-in >1 ABI. */
static int concurrent_nonparking_admission_stress() {
    constexpr int carriers = 4;
    constexpr int per_carrier = 128;
    constexpr int max_attempts = 100000;
    OresActorI64Heap* heap = ores_actor_i64_heap_open(990, 1, 2);
    std::atomic<int> executing{0};
    std::atomic<int> peak{0};
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    std::array<std::thread, carriers> threads;
    for (auto& worker : threads) {
        worker = std::thread([&] {
            for (int iteration = 0; iteration < per_carrier; ++iteration) {
                OresActorTurn turn{77, 88};
                OresActorTryEnterResult result = ORES_ACTOR_TRY_LOCK_BUSY;
                bool admitted = false;
                for (int attempt = 0; attempt < max_attempts; ++attempt) {
                    result = ores_actor_i64_heap_try_enter(heap, 990, 1, &turn);
                    if (result == ORES_ACTOR_TRY_ENTERED) {
                        admitted = true;
                        break;
                    }
                    if (turn.domain != 0 || turn.epoch != 0) ++failures;
                    if (result != ORES_ACTOR_TRY_LOCK_BUSY
                            && result != ORES_ACTOR_TRY_ACTOR_ACTIVE) {
                        ++failures;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!admitted) { ++failures; return; }
                const int active = executing.fetch_add(1) + 1;
                if (active != 1) ++failures;
                int observed = peak.load();
                while (active > observed && !peak.compare_exchange_weak(observed, active)) { }
                if (turn.domain == 0 || turn.epoch == 0) ++failures;
                std::this_thread::yield();
                executing.fetch_sub(1);
                ores_actor_i64_heap_leave(heap, turn);
                ++completed;
            }
        });
    }
    for (auto& worker : threads) worker.join();
    int failed = failures.load() || peak.load() != 1
        || completed.load() != carriers * per_carrier || executing.load() != 0;
    ores_actor_i64_heap_retire(heap);
    if (!ores_actor_i64_heap_try_destroy(heap)) failed = 1;
    return failed;
}

int main(void) {
    CHECK(concurrent_nonparking_admission_stress() == 0);
    /* One actor heap survives between logical turns and native carriers. */
    OresActorI64Heap* heap = ores_actor_i64_heap_open(29, 7, 2);
    OresActorTurn first = ores_actor_i64_heap_enter(heap, 29, 7);
    OresActorSlot saved = ores_actor_i64_heap_new(heap, first, 40);
    CHECK(ores_actor_i64_heap_read(heap, first, saved) == 40);
    OresActorLoan suspended = ores_actor_i64_heap_borrow(heap, first, saved);
    /* A live actor cannot take a second physical turn, even on its
     * current native carrier. Busy admission leaves its output untouched. */
    OresActorTurn denied = {0, 0};
    CHECK(ores_actor_i64_heap_try_enter(heap, 29, 7, &denied)
          == ORES_ACTOR_TRY_CARRIER_OCCUPIED);
    CHECK(denied.domain == 0 && denied.epoch == 0);
    ores_actor_i64_heap_leave(heap, first); /* simulates await/suspension */
    OresActorTurn next = {0, 0};
    CHECK(ores_actor_i64_heap_try_enter(heap, 29, 7, &next)
          == ORES_ACTOR_TRY_ENTERED);
    CHECK(next.domain == first.domain && next.epoch > first.epoch);
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

    /* Lifetime-only external owners block physical free without carrying any
     * slot/read authority. Release may happen on a callback carrier. */
    OresActorI64Heap* external_heap = ores_actor_i64_heap_open(41, 2, 2);
    OresActorTurn external_turn = ores_actor_i64_heap_enter(external_heap, 41, 2);
    (void) ores_actor_i64_heap_new(external_heap, external_turn, 123);
    OresActorExternalLease external =
        ores_actor_i64_heap_external_acquire(external_heap, external_turn);
    ores_actor_i64_heap_leave(external_heap, external_turn);
    ores_actor_i64_heap_retire(external_heap);
    CHECK(ores_actor_i64_heap_try_destroy(external_heap) == 0);
    std::thread callback_release([&] {
        ores_actor_i64_heap_external_release(external_heap, &external);
        ores_actor_i64_heap_external_release(external_heap, &external);
    });
    callback_release.join();
    CHECK(external.domain == 0 && external.index == 0 && external.generation == 0);
    CHECK(ores_actor_i64_heap_try_destroy(external_heap) == 1);
    external_heap = nullptr;
    /* The consumed token can be retried after CONTROL has destroyed/cleared
     * the heap pointer: release must return before touching heap storage. */
    ores_actor_i64_heap_external_release(external_heap, &external);

    /* Reusing a bounded lease slot must not let an old cleanup token release
     * the newer generation occupying that slot. */
    OresActorI64Heap* reused_heap = ores_actor_i64_heap_open(42, 3, 1);
    OresActorTurn reused_turn = ores_actor_i64_heap_enter(reused_heap, 42, 3);
    OresActorExternalLease old_lease =
        ores_actor_i64_heap_external_acquire(reused_heap, reused_turn);
    OresActorExternalLease stale_copy = old_lease;
    ores_actor_i64_heap_external_release(reused_heap, &old_lease);
    CHECK(old_lease.domain == 0 && old_lease.index == 0 && old_lease.generation == 0);
    OresActorExternalLease current_lease =
        ores_actor_i64_heap_external_acquire(reused_heap, reused_turn);
    CHECK(current_lease.index == stale_copy.index);
    CHECK(current_lease.generation > stale_copy.generation);
    ores_actor_i64_heap_external_release(reused_heap, &stale_copy);
    CHECK(stale_copy.domain == 0 && stale_copy.index == 0 && stale_copy.generation == 0);
    ores_actor_i64_heap_leave(reused_heap, reused_turn);
    ores_actor_i64_heap_retire(reused_heap);
    OresActorTurn retired_denied = {0, 0};
    CHECK(ores_actor_i64_heap_try_enter(reused_heap, 42, 3, &retired_denied)
          == ORES_ACTOR_TRY_RETIRED);
    CHECK(retired_denied.domain == 0 && retired_denied.epoch == 0);
    CHECK(ores_actor_i64_heap_try_destroy(reused_heap) == 0);
    ores_actor_i64_heap_external_release(reused_heap, &current_lease);
    CHECK(current_lease.domain == 0 && current_lease.index == 0
          && current_lease.generation == 0);
    CHECK(ores_actor_i64_heap_try_destroy(reused_heap) == 1);
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
    CHECK(rejects(FOREIGN_CARRIER_TURN));
    CHECK(rejects(NESTED_CARRIER_ACTOR));
    CHECK(rejects(FORGED_EXTERNAL_LEASE));
    CHECK(rejects(FORGED_EXTERNAL_DOMAIN));
    CHECK(rejects(EXTERNAL_AFTER_RETIRE));
#endif
    /* Domain authority must remain unique across repeated actor incarnation
     * creation/destruction. This exercises the saturating CAS issuance path
     * under ordinary operation; exhaustion itself is intentionally unreachable
     * in a bounded regression. */
    uint64_t previous_domain = 0;
    for (uint64_t i = 0; i < 1024; ++i) {
        OresActorI64Heap* probe = ores_actor_i64_heap_open(1000 + i, 1, 1);
        OresActorTurn probe_turn = ores_actor_i64_heap_enter(probe, 1000 + i, 1);
        CHECK(probe_turn.domain != 0);
        CHECK(probe_turn.domain > previous_domain);
        previous_domain = probe_turn.domain;
        ores_actor_i64_heap_leave(probe, probe_turn);
        ores_actor_i64_heap_retire(probe);
        CHECK(ores_actor_i64_heap_try_destroy(probe) == 1);
    }
    return 0;
}
