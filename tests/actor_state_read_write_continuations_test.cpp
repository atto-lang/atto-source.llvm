#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <stdexcept>
#include <thread>
#include <vector>

using Bridge = oreslang::runtime::ActorStateReadWriteContinuations;
using Status = Bridge::Status;
using Mode = Bridge::Mode;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

int main() {
    Bridge bridge;
    CHECK(bridge.request(1, Mode::Read).status == Status::Ready);
    CHECK(bridge.admitted(1, Mode::Read));
    CHECK(bridge.request(2, Mode::Write).status == Status::Queued);
    CHECK(bridge.request(3, Mode::Read).status == Status::Queued);
    CHECK(bridge.pending_notifications().empty());
    CHECK(!bridge.admitted(2, Mode::Write));
    bridge.complete(1);
    auto messages = bridge.pending_notifications();
    CHECK(messages.size() == 1);
    CHECK(messages[0].continuation_id == 2);
    CHECK(messages[0].status == Status::Ready);
    CHECK(!bridge.admitted(2, Mode::Write)); // CONTROL not yet accepted
    CHECK(!bridge.acknowledge(2, Status::Cancelled));
    CHECK(bridge.acknowledge(2, Status::Ready));
    CHECK(bridge.admitted(2, Mode::Write));
    CHECK(bridge.pending_notifications().empty());
    bridge.complete(2);
    CHECK(bridge.pending_notifications().size() == 1);
    CHECK(bridge.pending_notifications().front().continuation_id == 3);
    CHECK(bridge.acknowledge(3, Status::Ready));
    CHECK(bridge.admitted(3, Mode::Read));
    bridge.complete(3);

    // Cancelling a queued writer publishes one durable terminal receipt.
    CHECK(bridge.request(4, Mode::Read).status == Status::Ready);
    CHECK(bridge.request(5, Mode::Write).status == Status::Queued);
    CHECK(bridge.request(6, Mode::Read).status == Status::Queued);
    bridge.cancel_queued(5);
    messages = bridge.pending_notifications();
    CHECK(messages.size() == 2);
    CHECK(messages[0].continuation_id == 5 && messages[0].status == Status::Cancelled);
    CHECK(messages[1].continuation_id == 6 && messages[1].status == Status::Ready);
    CHECK(!bridge.acknowledge(5, Status::Ready));
    CHECK(bridge.acknowledge(5, Status::Cancelled));
    CHECK(!bridge.acknowledge(5, Status::Cancelled));
    CHECK(bridge.acknowledge(6, Status::Ready));
    bridge.complete(4);
    bridge.complete(6);

    // Cancellation races a queued wake: stale receipts cannot authorize work.
    CHECK(bridge.request(7, Mode::Read).status == Status::Ready);
    CHECK(bridge.request(8, Mode::Write).status == Status::Queued);
    bridge.complete(7);
    CHECK(bridge.pending_notifications().size() == 1);
    bridge.retire();
    messages = bridge.pending_notifications();
    CHECK(messages.size() == 1);
    CHECK(messages.front().continuation_id == 8);
    CHECK(messages.front().status == Status::Cancelled);
    CHECK(!bridge.acknowledge(8, Status::Ready));
    CHECK(bridge.acknowledge(8, Status::Cancelled));
    CHECK(bridge.pending_notifications().empty());
    CHECK(!bridge.admitted(8, Mode::Write));

    bool rejected = false;
    try { (void)bridge.request(9, Mode::Read); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);

    // A one-way continuation ID avoids stale CONTROL receipt ABA.
    Bridge monotonic;
    CHECK(monotonic.request(100, Mode::Read).status == Status::Ready);
    monotonic.complete(100);
    rejected = false;
    try { (void)monotonic.request(100, Mode::Read); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);

    // Cancellation backpressure must not create unbounded retained
    // continuation records while the CONTROL channel is unavailable.
    Registry bounded;
    CHECK(bounded.request(1, Mode::Read).status == Status::Ready);
    for (std::uint64_t id = 2;
         id <= Registry::max_outstanding_continuations; ++id) {
        CHECK(bounded.request(id, Mode::Write).status == Status::Queued);
        bounded.cancel_queued(id);
    }
    CHECK(bounded.pending_notifications().size() ==
          Registry::max_outstanding_continuations - 1);
    bool saturated = false;
    try {
        (void)bounded.request(Registry::max_outstanding_continuations + 1,
                              Mode::Write);
    } catch (const std::overflow_error&) {
        saturated = true;
    }
    CHECK(saturated);
    for (std::uint64_t id = 2;
         id <= Registry::max_outstanding_continuations; ++id) {
        CHECK(bounded.acknowledge(id, Status::Cancelled));
    }
    CHECK(bounded.request(Registry::max_outstanding_continuations + 1,
                          Mode::Write).status == Status::Queued);
    bounded.retire();
    CHECK(bounded.pending_notifications().size() == 2);

    // Publication observation is retryable and side-effect free.
    Bridge retryable;
    CHECK(retryable.request(1, Mode::Read).status == Status::Ready);
    CHECK(retryable.request(2, Mode::Write).status == Status::Queued);
    retryable.complete(1);
    for (int i = 0; i < 20; ++i) {
        CHECK(retryable.pending_notifications().size() == 1);
        CHECK(!retryable.admitted(2, Mode::Write));
    }
    CHECK(retryable.acknowledge(2, Status::Ready));
    CHECK(retryable.admitted(2, Mode::Write));
    retryable.complete(2);
    return 0;
}
