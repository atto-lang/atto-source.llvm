#include "oreslang/runtime/SerialActorLifetimeGate.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::SerialActorLifetimeGate;
using Kind = SerialActorLifetimeGate::Kind;
using Status = SerialActorLifetimeGate::SubmitStatus;
using Lifecycle = SerialActorLifetimeGate::Lifecycle;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

int main() {
    SerialActorLifetimeGate gate(42, 9, 1000);
    CHECK(gate.lifecycle() == Lifecycle::Idle);
    const auto first = gate.submit(Kind::Http, 1);
    CHECK(first.status == Status::Admitted && first.lease);
    const auto held = *first.lease;
    CHECK(gate.lifecycle() == Lifecycle::Running);
    CHECK(gate.may_execute(held));
    CHECK(gate.suspend(held));
    CHECK(gate.lifecycle() == Lifecycle::Suspended);
    CHECK(gate.admitted(held));
    CHECK(!gate.may_execute(held));
    CHECK(!gate.suspend(held));
    const auto msg = gate.submit(Kind::Message, 2);
    const auto req = gate.submit(Kind::Http, 3);
    CHECK(msg.status == Status::Queued && req.status == Status::Queued);
    CHECK(gate.busy() && gate.queued() == 2);
    CHECK(gate.admitted(held));
    CHECK(gate.resume(held));
    CHECK(gate.lifecycle() == Lifecycle::Running);
    CHECK(gate.may_execute(held));
    CHECK(!gate.resume(held));
    CHECK(gate.submit(Kind::Message, 2).status == Status::Duplicate);
    CHECK(gate.cancel_pending(3));
    CHECK(!gate.cancel_pending(3));
    CHECK(gate.queued() == 1);

    auto finished = gate.finish(held);
    CHECK(finished.accepted && finished.next);
    CHECK(finished.next->kind == Kind::Message);
    CHECK(finished.next->event_id == 2);
    CHECK(finished.next->generation > held.generation);
    CHECK(!gate.admitted(held));
    CHECK(!gate.may_execute(held));
    CHECK(!gate.finish(held).accepted);
    CHECK(!gate.suspend(held));
    CHECK(gate.finish(*finished.next).accepted);
    CHECK(!gate.busy());
    CHECK(gate.lifecycle() == Lifecycle::Idle);
    const auto stopped = gate.stop();
    CHECK(gate.lifecycle() == Lifecycle::Stopped);
    CHECK(!stopped.revoked);
    CHECK(stopped.rejected.empty());
    CHECK(!gate.admitted(*finished.next));
    CHECK(gate.submit(Kind::Http, 4).status == Status::Stopped);

    // Other incarnations cannot reuse a prior incarnation's ticket.
    SerialActorLifetimeGate fresh(42, 10, 1000);
    CHECK(!fresh.admitted(held));
    CHECK(!fresh.may_execute(held));
    const auto newTicket = fresh.submit(Kind::Message, 1);
    CHECK(newTicket.status == Status::Admitted && newTicket.lease);
    CHECK(newTicket.lease->incarnation == 10);
    CHECK(!fresh.finish(held).accepted);
    CHECK(fresh.may_execute(*newTicket.lease));
    CHECK(fresh.finish(*newTicket.lease).accepted);
    CHECK(!fresh.may_execute(*newTicket.lease));

    // Two independently created gates with identical *public* identity,
    // generation, event ID and kind must never recognize one another's leases.
    // This also covers accidental incarnation reuse during a failed restart.
    SerialActorLifetimeGate twinA(91, 7, 2);
    SerialActorLifetimeGate twinB(91, 7, 2);
    const auto a = twinA.submit(Kind::Http, 1);
    const auto b = twinB.submit(Kind::Http, 1);
    CHECK(a.lease && b.lease);
    CHECK(a.lease->actor_id == b.lease->actor_id);
    CHECK(a.lease->incarnation == b.lease->incarnation);
    CHECK(a.lease->generation == b.lease->generation);
    CHECK(a.lease->event_id == b.lease->event_id);
    CHECK(a.lease->kind == b.lease->kind);
    CHECK(!twinA.admitted(*b.lease));
    CHECK(!twinB.may_execute(*a.lease));
    CHECK(!twinA.suspend(*b.lease));
    CHECK(!twinA.resume(*b.lease));
    CHECK(!twinA.finish(*b.lease).accepted);
    CHECK(twinA.may_execute(*a.lease));
    CHECK(twinB.may_execute(*b.lease));
    CHECK(twinA.finish(*a.lease).accepted);
    CHECK(twinB.finish(*b.lease).accepted);
    // An application cannot construct a ticket simply by guessing its fields.
    SerialActorLifetimeGate::Lease fabricated{};
    fabricated.actor_id = 91;
    fabricated.incarnation = 7;
    fabricated.generation = 1;
    fabricated.event_id = 1;
    fabricated.kind = Kind::Http;
    SerialActorLifetimeGate forgedGate(91, 7, 2);
    const auto minted = forgedGate.submit(Kind::Http, 1);
    CHECK(minted.lease);
    CHECK(!forgedGate.admitted(fabricated));
    CHECK(!forgedGate.finish(fabricated).accepted);
    CHECK(forgedGate.may_execute(*minted.lease));
    CHECK(forgedGate.finish(*minted.lease).accepted);

    // Mixed concurrent enqueuers: exactly one actor-wide lifetime token.
    SerialActorLifetimeGate many(700, 1, 1000);
    auto leader = many.submit(Kind::Http, 1);
    CHECK(leader.status == Status::Admitted);
    std::atomic<int> queued{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> producers;
    for (int thread = 0; thread < 4; ++thread) {
        producers.emplace_back([thread, &many, &queued, &failed]() {
            for (int j = 0; j < 250; ++j) {
                std::uint64_t id = 2 + static_cast<std::uint64_t>(thread * 250 + j);
                auto kind = (id & 1) == 0 ? Kind::Http : Kind::Message;
                auto result = many.submit(kind, id);
                if (result.status == Status::Queued) ++queued;
                else ++failed;
            }
        });
    }
    for (auto& producer : producers) producer.join();
    CHECK(failed == 0);
    CHECK(queued == 1000);
    CHECK(many.queued() == 1000);
    CHECK(many.busy());
    CHECK(many.submit(Kind::Message, 2001).status == Status::Full);
    std::uint64_t count = 1;
    auto current = *leader.lease;
    while (true) {
        CHECK(many.admitted(current));
        const auto advanced = many.finish(current);
        CHECK(advanced.accepted);
        ++count;
        if (!advanced.next) break;
        CHECK(!many.admitted(current));
        current = *advanced.next;
        CHECK(current.kind == ((current.event_id & 1) == 0 ? Kind::Http : Kind::Message));
    }
    CHECK(count == 1002); // 1 leader + 1000 queued + final loop iteration
    CHECK(!many.busy() && many.queued() == 0);
    many.stop();

    // Stop returns both in-flight and pending tokens so the scheduler may
    // notify every waiter instead of silently dropping queued HTTP requests.
    SerialActorLifetimeGate notify(801, 6, 3);
    const auto owned = notify.submit(Kind::Message, 11);
    CHECK(owned.lease);
    CHECK(notify.submit(Kind::Http, 12).status == Status::Queued);
    CHECK(notify.submit(Kind::Message, 13).status == Status::Queued);
    auto rejected = notify.stop();
    CHECK(rejected.revoked);
    CHECK(rejected.revoked->event_id == 11);
    CHECK(rejected.rejected.size() == 2);
    CHECK(rejected.rejected[0].event_id == 12);
    CHECK(rejected.rejected[1].event_id == 13);
    CHECK(!notify.may_execute(*owned.lease));
    CHECK(!notify.stop().revoked);

    // Stop must revoke leases atomically against concurrent submission.
    for (int trial = 0; trial < 100; ++trial) {
        SerialActorLifetimeGate racing(1000 + static_cast<std::uint64_t>(trial), 1, 256);
        const auto inFlight = racing.submit(Kind::Http, 1);
        CHECK(inFlight.lease);
        std::atomic<bool> released{false};
        std::thread producer([&racing, &released]() {
            for (std::uint64_t id = 2; id < 202; ++id) {
                const auto answer = racing.submit(Kind::Message, id);
                CHECK(answer.status == Status::Queued || answer.status == Status::Stopped);
            }
            released = true;
        });
        racing.stop();
        producer.join();
        CHECK(released.load());
        CHECK(!racing.busy() && racing.queued() == 0);
        CHECK(!racing.admitted(*inFlight.lease));
        CHECK(!racing.may_execute(*inFlight.lease));
        CHECK(!racing.finish(*inFlight.lease).accepted);
        CHECK(racing.submit(Kind::Http, 300).status == Status::Stopped);
    }
    return 0;
}
