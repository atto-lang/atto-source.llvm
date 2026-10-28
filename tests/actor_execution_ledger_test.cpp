#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
using Ledger = oreslang::runtime::ActorExecutionLedger<4>;
using Status = Ledger::Status;
using Kind = Ledger::Kind;
int main() {
    Ledger actor(42, 7);
    Ledger::EventId a{}, b{}, timer{}, control{}, pick{};
    assert(actor.admit(Kind::Message, a) == Status::Ok);
    assert(actor.claim(a, false) == Status::Unauthorized);
    assert(actor.select(pick) == Status::Ok && pick.sequence == a.sequence);
    assert(actor.claim(a, true) == Status::Ok);
    assert(actor.admit(Kind::Message, b) == Status::Ok);
    assert(actor.admit(Kind::Timer, timer) == Status::Ok);
    assert(actor.select(pick) == Status::Busy);
    assert(actor.suspend(a) == Status::Ok);
    assert(actor.has_unbounded_suspension_blocker());
    assert(actor.select(pick) == Status::Blocked); // B does not overtake A
    assert(actor.notify_ready(timer) == Status::Ok);
    assert(actor.select(pick) == Status::Blocked);
    assert(actor.admit(Kind::Control, control) == Status::Ok);
    assert(actor.select(pick) == Status::Ok && pick.sequence == control.sequence);
    assert(actor.claim(control, true) == Status::Ok);
    assert(actor.claim(b, true) == Status::Busy);
    assert(actor.finish(control) == Status::Ok);
    assert(actor.select(pick) == Status::Blocked);
    assert(actor.notify_ready(a) == Status::Ok);
    assert(!actor.has_unbounded_suspension_blocker());
    assert(actor.notify_ready(a) == Status::AlreadyReady);
    assert(actor.select(pick) == Status::Ok && pick.sequence == a.sequence);
    assert(actor.claim(b, true) == Status::Blocked);
    assert(actor.claim(a, true) == Status::Ok);
    assert(actor.finish(a) == Status::Ok);
    assert(actor.select(pick) == Status::Ok && pick.sequence == b.sequence);
    assert(actor.claim(b, true) == Status::Ok);
    assert(actor.finish(b) == Status::Ok);
    assert(actor.select(pick) == Status::Ok && pick.sequence == timer.sequence);
    assert(actor.claim(timer, true) == Status::Ok);
    assert(actor.finish(timer) == Status::Ok);
    assert(actor.select(pick) == Status::Idle);
    assert(actor.snapshot().live == 0);
    assert(actor.notify_ready(a) == Status::Stale);
    assert(actor.admit(Kind::Message, a) == Status::Ok);
    Ledger::EventId stale = a;
    stale.actor_generation++;
    assert(actor.notify_ready(stale) == Status::Stale);
    assert(actor.cancel(a) == Status::Ok);
    assert(actor.cancel(a) == Status::Stale);
    assert(actor.stop() == 0);
    assert(actor.physically_retired());
    assert(actor.admit(Kind::Message, a) == Status::Closed);
    Ledger replacement(42, 8);
    assert(replacement.admit(Kind::Message, b) == Status::Ok);
    assert(replacement.claim(a, true) == Status::Stale);
    assert(replacement.claim(b, true) == Status::Ok);
    assert(replacement.stop() == 0); // running turn not yet quiescent
    assert(!replacement.physically_retired());
    assert(replacement.finish(b) == Status::Ok);
    assert(replacement.physically_retired());
    assert(replacement.notify_ready(b) == Status::Stale);

    // Mandatory supervisor CONTROL survives STOP and is never cancelled by
    // the ordinary event-cancellation authority. Retire only after it runs.
    Ledger stopping(90, 1);
    Ledger::EventId normal{}, urgent_after_stop{};
    assert(stopping.admit(Kind::Message, normal) == Status::Ok);
    assert(stopping.claim(normal, true) == Status::Ok);
    assert(stopping.admit(Kind::Control, urgent_after_stop) == Status::Ok);
    assert(stopping.stop() == 0);
    assert(!stopping.physically_retired());
    assert(stopping.cancel(urgent_after_stop) == Status::Unauthorized);
    assert(stopping.finish(normal) == Status::Ok);
    assert(!stopping.physically_retired());
    assert(stopping.select(pick) == Status::Ok &&
           pick.sequence == urgent_after_stop.sequence);
    assert(stopping.claim(urgent_after_stop, true) == Status::Ok);
    assert(stopping.finish(urgent_after_stop) == Status::Ok);
    assert(stopping.physically_retired());

    // A timer registration is not a logical mailbox barrier until expiry.
    Ledger timer_first(5, 3);
    assert(timer_first.admit(Kind::Timer, timer) == Status::Ok);
    assert(timer_first.admit(Kind::Message, a) == Status::Ok);
    assert(timer_first.select(pick) == Status::Ok && pick.sequence == a.sequence);
    assert(timer_first.claim(a, true) == Status::Ok);
    assert(timer_first.finish(a) == Status::Ok);
    assert(timer_first.select(pick) == Status::Idle);
    assert(timer_first.notify_ready(timer) == Status::Ok);
    assert(timer_first.select(pick) == Status::Ok && pick.sequence == timer.sequence);
    std::cout << "ACTOR_EXECUTION_LEDGER_PASS\n";
}
