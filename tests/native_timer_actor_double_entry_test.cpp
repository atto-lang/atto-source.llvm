#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/HardRealtimeTimerQueue.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
using Ledger = oreslang::runtime::ActorExecutionLedger<4>;
using Pool = oreslang::runtime::realtime::HardRealtimeTimerQueue<3>;
int main() {
    // Physically independent actor-local and scheduler CONTROL structures.
    Ledger actor(42, 7);
    Pool clock;
    Ledger::EventId a{}, timer{}, b{}, urgent{}, pick{};
    Pool::Handle clock_ticket{};
    Pool::Expired due{};
    Pool::Start start{};
    assert(actor.admit(Ledger::Kind::Message, a) == Ledger::Status::Ok);
    assert(actor.claim(a, true) == Ledger::Status::Ok);
    assert(actor.suspend(a) == Ledger::Status::Ok);
    assert(actor.admit(Ledger::Kind::Timer, timer) == Ledger::Status::Ok);
    assert(clock.arm_after(100, 5, 42, 7, 4, clock_ticket) == Pool::Status::Ok);
    assert(actor.admit(Ledger::Kind::Message, b) == Ledger::Status::Ok);
    assert(clock.poll_due(105, due) == Pool::Status::Ok);
    assert(due.actor_id == 42 && due.actor_generation == 7);
    assert(due.handle.domain == clock_ticket.domain);
    // Actor lease is acquired before consuming scheduler's opaque due notice.
    assert(actor.notify_ready(timer) == Ledger::Status::Ok);
    assert(actor.select(pick) == Ledger::Status::Blocked); // A still suspended
    assert(actor.admit(Ledger::Kind::Control, urgent) == Ledger::Status::Ok);
    assert(actor.select(pick) == Ledger::Status::Ok && pick.sequence == urgent.sequence);
    assert(actor.claim(urgent, true) == Ledger::Status::Ok);
    assert(actor.finish(urgent) == Ledger::Status::Ok);
    assert(actor.notify_ready(a) == Ledger::Status::Ok);
    assert(actor.select(pick) == Ledger::Status::Ok && pick.sequence == a.sequence);
    assert(actor.claim(a, true) == Ledger::Status::Ok);
    assert(actor.finish(a) == Ledger::Status::Ok);
    assert(actor.select(pick) == Ledger::Status::Ok && pick.sequence == timer.sequence);
    // Timer was admitted BEFORE B, so the timer wins when A completes.
    assert(actor.claim(timer, true) == Ledger::Status::Ok);
    assert(clock.claim_start(clock_ticket, 42, 7, 112, start)
           == Pool::Status::DeadlineMissed); // no false RT success
    assert(start.lateness_ns == 7 && start.maximum_lateness_ns == 4);
    assert(actor.finish(timer) == Ledger::Status::Ok); // physical host hook returned
    assert(actor.select(pick) == Ledger::Status::Ok && pick.sequence == b.sequence);
    assert(actor.claim(b, true) == Ledger::Status::Ok);
    assert(actor.finish(b) == Ledger::Status::Ok);
    assert(actor.stop() == 0 && actor.physically_retired());
    assert(actor.notify_ready(timer) == Ledger::Status::Stale);

    // Old scheduler token cannot ignite replacement incarnation after STOP.
    Ledger newer(42, 8);
    Ledger::EventId replacement{};
    Pool::Handle old_clock{};
    assert(clock.arm_after(112, 3, 42, 7, 1, old_clock) == Pool::Status::Ok);
    std::size_t revoked = 0;
    assert(clock.revoke_actor(42, 7, revoked) == Pool::Status::Ok && revoked == 1);
    assert(clock.poll_due(115, due) == Pool::Status::NotReady);
    assert(newer.admit(Ledger::Kind::Message, replacement) == Ledger::Status::Ok);
    assert(newer.notify_ready(timer) == Ledger::Status::Stale);
    assert(newer.claim(timer, true) == Ledger::Status::Stale);
    assert(clock.claim_start(old_clock, 42, 7, 115, start) == Pool::Status::Stale);
    std::cout << "NATIVE_TIMER_ACTOR_DOUBLE_ENTRY_PASS\n";
}
