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
    CHECK(bridge.request(bridge.incarnation(), 1, Mode::Read).status == Status::Ready);
    CHECK(bridge.admitted(bridge.incarnation(), 1, Mode::Read));
    CHECK(bridge.request(bridge.incarnation(), 2, Mode::Write).status == Status::Queued);
    CHECK(bridge.request(bridge.incarnation(), 3, Mode::Read).status == Status::Queued);
    CHECK(bridge.pending_notifications().empty());
    CHECK(!bridge.admitted(bridge.incarnation(), 2, Mode::Write));
    bridge.complete(bridge.incarnation(), 1);
    auto messages = bridge.pending_notifications();
    CHECK(messages.size() == 1);
    CHECK(messages[0].continuation_id == 2);
    CHECK(messages[0].status == Status::Ready);
    CHECK(!bridge.admitted(bridge.incarnation(), 2, Mode::Write)); // CONTROL not yet accepted
    CHECK(!bridge.acknowledge({bridge.incarnation(), 2, Status::Cancelled}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 2, Status::Ready}));
    CHECK(bridge.admitted(bridge.incarnation(), 2, Mode::Write));
    CHECK(bridge.pending_notifications().empty());
    bridge.complete(bridge.incarnation(), 2);
    CHECK(bridge.pending_notifications().size() == 1);
    CHECK(bridge.pending_notifications().front().continuation_id == 3);
    CHECK(bridge.acknowledge({bridge.incarnation(), 3, Status::Ready}));
    CHECK(bridge.admitted(bridge.incarnation(), 3, Mode::Read));
    bridge.complete(bridge.incarnation(), 3);

    // Cancelling a queued writer publishes one durable terminal receipt.
    CHECK(bridge.request(bridge.incarnation(), 4, Mode::Read).status == Status::Ready);
    CHECK(bridge.request(bridge.incarnation(), 5, Mode::Write).status == Status::Queued);
    CHECK(bridge.request(bridge.incarnation(), 6, Mode::Read).status == Status::Queued);
    bridge.cancel_queued(bridge.incarnation(), 5);
    messages = bridge.pending_notifications();
    CHECK(messages.size() == 2);
    CHECK(messages[0].continuation_id == 5 && messages[0].status == Status::Cancelled);
    CHECK(messages[1].continuation_id == 6 && messages[1].status == Status::Ready);
    CHECK(!bridge.acknowledge({bridge.incarnation(), 5, Status::Ready}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 5, Status::Cancelled}));
    CHECK(!bridge.acknowledge({bridge.incarnation(), 5, Status::Cancelled}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 6, Status::Ready}));
    bridge.complete(bridge.incarnation(), 4);
    bridge.complete(bridge.incarnation(), 6);

    // Cancellation races a queued wake: stale receipts cannot authorize work.
    CHECK(bridge.request(bridge.incarnation(), 7, Mode::Read).status == Status::Ready);
    CHECK(bridge.request(bridge.incarnation(), 8, Mode::Write).status == Status::Queued);
    bridge.complete(bridge.incarnation(), 7);
    CHECK(bridge.pending_notifications().size() == 1);
    bridge.retire();
    messages = bridge.pending_notifications();
    CHECK(messages.size() == 1);
    CHECK(messages.front().continuation_id == 8);
    CHECK(messages.front().status == Status::Cancelled);
    CHECK(!bridge.acknowledge({bridge.incarnation(), 8, Status::Ready}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 8, Status::Cancelled}));
    CHECK(bridge.pending_notifications().empty());
    CHECK(!bridge.admitted(bridge.incarnation(), 8, Mode::Write));

    bool rejected = false;
    try { (void)bridge.request(bridge.incarnation(), 9, Mode::Read); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);

    // A one-way continuation ID avoids stale CONTROL receipt ABA.
    Bridge monotonic;
    CHECK(monotonic.request(monotonic.incarnation(), 100, Mode::Read).status == Status::Ready);
    monotonic.complete(monotonic.incarnation(), 100);
    rejected = false;
    try { (void)monotonic.request(monotonic.incarnation(), 100, Mode::Read); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);

    // Cancellation backpressure must not create unbounded retained
    // continuation records while the CONTROL channel is unavailable.
    Bridge bounded;
    CHECK(bounded.request(bounded.incarnation(), 1, Mode::Read).status == Status::Ready);
    for (std::uint64_t id = 2;
         id <= Bridge::max_outstanding_continuations; ++id) {
        CHECK(bounded.request(bounded.incarnation(), id, Mode::Write).status == Status::Queued);
        bounded.cancel_queued(bounded.incarnation(), id);
    }
    CHECK(bounded.pending_notifications().size() ==
          Bridge::max_outstanding_continuations - 1);
    bool saturated = false;
    try {
        (void)bounded.request(bounded.incarnation(), Bridge::max_outstanding_continuations + 1,
                              Mode::Write);
    } catch (const std::overflow_error&) {
        saturated = true;
    }
    CHECK(saturated);
    for (std::uint64_t id = 2;
         id <= Bridge::max_outstanding_continuations; ++id) {
        CHECK(bounded.acknowledge({bounded.incarnation(), id, Status::Cancelled}));
    }
    CHECK(bounded.request(bounded.incarnation(), Bridge::max_outstanding_continuations + 1,
                          Mode::Write).status == Status::Queued);
    bounded.retire();
    CHECK(bounded.pending_notifications().size() == 2);

    // Publication observation is retryable and side-effect free.
    Bridge retryable;
    CHECK(retryable.request(retryable.incarnation(), 1, Mode::Read).status == Status::Ready);
    CHECK(retryable.request(retryable.incarnation(), 2, Mode::Write).status == Status::Queued);
    retryable.complete(retryable.incarnation(), 1);
    for (int i = 0; i < 20; ++i) {
        CHECK(retryable.pending_notifications().size() == 1);
        CHECK(!retryable.admitted(retryable.incarnation(), 2, Mode::Write));
    }
    CHECK(retryable.acknowledge({retryable.incarnation(), 2, Status::Ready}));
    CHECK(retryable.admitted(retryable.incarnation(), 2, Mode::Write));
    retryable.complete(retryable.incarnation(), 2);
    return 0;
}
