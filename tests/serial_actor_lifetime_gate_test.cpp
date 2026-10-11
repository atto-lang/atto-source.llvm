#include "oreslang/runtime/SerialActorLifetimeGate.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::SerialActorLifetimeGate;
using Kind = SerialActorLifetimeGate::Kind;
using Status = SerialActorLifetimeGate::SubmitStatus;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

int main() {
    SerialActorLifetimeGate gate(42, 9, 1000);
    const auto first = gate.submit(Kind::Http, 1);
    CHECK(first.status == Status::Admitted && first.lease);
    const auto held = *first.lease;
    CHECK(gate.may_execute(held));
    CHECK(gate.suspend(held));
    CHECK(gate.admitted(held));
    CHECK(!gate.may_execute(held));
    CHECK(!gate.suspend(held));
    const auto msg = gate.submit(Kind::Message, 2);
    const auto req = gate.submit(Kind::Http, 3);
    CHECK(msg.status == Status::Queued && req.status == Status::Queued);
    CHECK(gate.busy() && gate.queued() == 2);
    CHECK(gate.admitted(held));
    CHECK(gate.resume(held));
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
    gate.stop();
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
    return 0;
}
