#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Ledger = oreslang::runtime::ActorExecutionLedger<4>;
using Pool = oreslang::runtime::realtime::HardRealtimeTimerQueue<4>;

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "RT_TIMER_ACTOR_HEAP_FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #x); std::abort(); \
} } while (0)

static void timely_resume_preserves_physical_loan_and_peer() {
    constexpr std::uint64_t actor_a = 7001;
    constexpr std::uint64_t generation_a = 7;
    constexpr std::uint64_t actor_b = 7002;
    constexpr std::uint64_t generation_b = 1;

    OresActorI64Heap* heap_a = ores_actor_i64_heap_open(actor_a, generation_a, 1);
    OresActorI64Heap* heap_b = ores_actor_i64_heap_open(actor_b, generation_b, 1);
    CHECK(heap_a && heap_b);

    OresActorTurn turn_a = ores_actor_i64_heap_enter(heap_a, actor_a, generation_a);
    OresActorSlot slot_a = ores_actor_i64_heap_new(heap_a, turn_a, 10);
    OresActorLoan loan_a = ores_actor_i64_heap_borrow(heap_a, turn_a, slot_a);
    ores_actor_i64_heap_leave(heap_a, turn_a);

    OresActorTurn turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    OresActorSlot slot_b = ores_actor_i64_heap_new(heap_b, turn_b, 3);
    ores_actor_i64_heap_leave(heap_b, turn_b);

    Ledger actor(actor_a, generation_a);
    Pool timers;
    Ledger::Token a{};
    Ledger::Selection selected{};
    Pool::Handle timer{};
    Pool::Expired due{};
    Pool::Start start{};

    CHECK(actor.admit(Ledger::Kind::Mail, a) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(actor.prepare_await(a) == Ledger::Status::Ok);
    CHECK(timers.arm_after(100, 5, actor_a, generation_a, 2, timer)
          == Pool::Status::Ok);
    CHECK(actor.suspend(a) == Ledger::Status::Ok);

    // A's shared loan survives suspension while B's physically separate heap
    // remains writable and independently owned.
    turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    ores_actor_i64_heap_write(heap_b, turn_b, slot_b, 31);
    CHECK(ores_actor_i64_heap_read(heap_b, turn_b, slot_b) == 31);
    ores_actor_i64_heap_leave(heap_b, turn_b);

    CHECK(timers.poll_due(105, due) == Pool::Status::Ok);
    CHECK(due.actor_id == actor_a && due.actor_generation == generation_a);
    CHECK(actor.wake(a) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);

    // Physical ownership cleanup occurs on the authoritative actor turn.
    // Only after the shared loan is released may the timer gate admit the
    // resumed guest segment and permit mutation of actor A's owned slot.
    ores_actor_i64_heap_release(heap_a, loan_a);
    CHECK(timers.claim_start(timer, actor_a, generation_a, 106, start)
          == Pool::Status::Ok);
    CHECK(start.lateness_ns == 1);

    turn_a = ores_actor_i64_heap_enter(heap_a, actor_a, generation_a);
    ores_actor_i64_heap_write(heap_a, turn_a, slot_a, 12);
    CHECK(ores_actor_i64_heap_read(heap_a, turn_a, slot_a) == 12);
    ores_actor_i64_heap_leave(heap_a, turn_a);
    CHECK(actor.finish_after_exit(a) == Ledger::Status::Ok);

    ores_actor_i64_heap_retire(heap_a);
    CHECK(ores_actor_i64_heap_try_destroy(heap_a) == 1);

    // B remains live and usable after A's physical heap has been destroyed.
    turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    CHECK(ores_actor_i64_heap_read(heap_b, turn_b, slot_b) == 31);
    ores_actor_i64_heap_write(heap_b, turn_b, slot_b, 32);
    CHECK(ores_actor_i64_heap_read(heap_b, turn_b, slot_b) == 32);
    ores_actor_i64_heap_leave(heap_b, turn_b);
    ores_actor_i64_heap_retire(heap_b);
    CHECK(ores_actor_i64_heap_try_destroy(heap_b) == 1);
}

static void stop_revokes_timer_and_reclaims_only_stopped_heap() {
    constexpr std::uint64_t actor_a = 8001;
    constexpr std::uint64_t generation_a = 9;
    constexpr std::uint64_t actor_b = 8002;
    constexpr std::uint64_t generation_b = 1;

    OresActorI64Heap* heap_a = ores_actor_i64_heap_open(actor_a, generation_a, 1);
    OresActorI64Heap* heap_b = ores_actor_i64_heap_open(actor_b, generation_b, 1);
    CHECK(heap_a && heap_b);

    OresActorTurn turn_a = ores_actor_i64_heap_enter(heap_a, actor_a, generation_a);
    OresActorSlot slot_a = ores_actor_i64_heap_new(heap_a, turn_a, 17);
    OresActorLoan loan_a = ores_actor_i64_heap_borrow(heap_a, turn_a, slot_a);
    ores_actor_i64_heap_leave(heap_a, turn_a);

    OresActorTurn turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    OresActorSlot slot_b = ores_actor_i64_heap_new(heap_b, turn_b, 40);
    ores_actor_i64_heap_leave(heap_b, turn_b);

    Ledger actor(actor_a, generation_a);
    Pool timers;
    Ledger::Token a{};
    Ledger::Selection selected{};
    Pool::Handle old_timer{}, replacement_timer{};
    Pool::Expired due{};
    Pool::Start start{};
    std::size_t revoked = 0;

    CHECK(actor.admit(Ledger::Kind::Mail, a) == Ledger::Status::Ok);
    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(actor.prepare_await(a) == Ledger::Status::Ok);
    CHECK(timers.arm_after(200, 5, actor_a, generation_a, 2, old_timer)
          == Pool::Status::Ok);
    CHECK(actor.suspend(a) == Ledger::Status::Ok);

    turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    ores_actor_i64_heap_write(heap_b, turn_b, slot_b, 41);
    ores_actor_i64_heap_leave(heap_b, turn_b);

    CHECK(actor.stop_and_request_cleanup() == Ledger::Status::Ok);
    CHECK(timers.revoke_actor(actor_a, generation_a, revoked) == Pool::Status::Ok);
    CHECK(revoked == 1);
    CHECK(timers.poll_due(205, due) == Pool::Status::NotReady);
    CHECK(timers.claim_start(old_timer, actor_a, generation_a, 205, start)
          == Pool::Status::Stale);
    CHECK(actor.wake(a) == Ledger::Status::AlreadyCancelled);

    // Logical STOP cannot reclaim native memory while the suspended loan is
    // still physically live.
    ores_actor_i64_heap_retire(heap_a);
    CHECK(ores_actor_i64_heap_try_destroy(heap_a) == 0);

    CHECK(actor.claim_under_lease(selected) == Ledger::Status::Ok);
    CHECK(selected.token.sequence == a.sequence);
    CHECK(selected.dispatch == Ledger::Dispatch::CancelCleanup);
    ores_actor_i64_heap_release(heap_a, loan_a);
    CHECK(actor.finish_after_exit(a) == Ledger::Status::Ok);
    CHECK(actor.physically_retired());
    CHECK(ores_actor_i64_heap_try_destroy(heap_a) == 1);

    // A replacement actor generation may use a new timer reservation, but the
    // old generation cannot claim it after A's physical heap is gone.
    CHECK(timers.arm_after(205, 0, actor_a, generation_a + 1, 0, replacement_timer)
          == Pool::Status::Ok);
    CHECK(timers.poll_due(205, due) == Pool::Status::Ok);
    CHECK(timers.claim_start(replacement_timer, actor_a, generation_a, 205, start)
          == Pool::Status::Stale);
    CHECK(timers.claim_start(replacement_timer, actor_a, generation_a + 1, 205, start)
          == Pool::Status::Ok);

    // B survives A's STOP and physical reclamation.
    turn_b = ores_actor_i64_heap_enter(heap_b, actor_b, generation_b);
    CHECK(ores_actor_i64_heap_read(heap_b, turn_b, slot_b) == 41);
    ores_actor_i64_heap_write(heap_b, turn_b, slot_b, 42);
    CHECK(ores_actor_i64_heap_read(heap_b, turn_b, slot_b) == 42);
    ores_actor_i64_heap_leave(heap_b, turn_b);
    ores_actor_i64_heap_retire(heap_b);
    CHECK(ores_actor_i64_heap_try_destroy(heap_b) == 1);
}

int main() {
    timely_resume_preserves_physical_loan_and_peer();
    stop_revokes_timer_and_reclaims_only_stopped_heap();
    std::puts("RT_TIMER_ACTOR_HEAP_INTEGRATION_PASS");
    return 0;
}
