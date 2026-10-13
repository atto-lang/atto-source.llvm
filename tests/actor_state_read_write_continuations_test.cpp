#define ORESLANG_RW_INTERNAL_TEST_HOOKS 1
#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <stdexcept>
#include <thread>
#include <vector>

using Bridge = oreslang::runtime::ActorStateReadWriteContinuations;
using Status = Bridge::Status;
using Mode = Bridge::Mode;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

// Test-only simulated successful CONTROL admission. Production code must
// enqueue an actual typed CONTROL notification before acknowledging READY.
static Bridge::Request request_with_control(Bridge& bridge,
                                             std::uint64_t id, Mode mode) {
    const auto result = bridge.request(bridge.incarnation(), id, mode);
    if (result.status == Status::Ready) {
        CHECK(!bridge.admitted(bridge.incarnation(), id, mode));
        CHECK(bridge.acknowledge({bridge.incarnation(), id, Status::Ready}));
        CHECK(bridge.admitted(bridge.incarnation(), id, mode));
    }
    return result;
}

int main() {
    // A newly minted ticket cannot bypass CONTROL simply because there was
    // no contention. Cancellation before its first receipt must be legal.
    Bridge first;
    CHECK(first.request(first.incarnation(), 1, Mode::Write).status == Status::Ready);
    CHECK(!first.admitted(first.incarnation(), 1, Mode::Write));
    CHECK(first.pending_notifications().size() == 1);
    bool blocked = false;
    try { first.complete_undispatched_for_test(first.incarnation(), 1); }
    catch (const std::logic_error&) { blocked = true; }
    CHECK(blocked);
    first.cancel_pending(first.incarnation(), 1);
    auto original = first.pending_notifications();
    CHECK(original.size() == 1 && original.front().status == Status::Cancelled);
    CHECK(!first.acknowledge({first.incarnation(), 1, Status::Ready}));
    CHECK(first.acknowledge(original.front()));
    CHECK(!first.admitted(first.incarnation(), 1, Mode::Write));
    CHECK(first.pending_notifications().empty());

    Bridge bridge;
    CHECK(request_with_control(bridge, 1, Mode::Read).status == Status::Ready);
    CHECK(bridge.admitted(bridge.incarnation(), 1, Mode::Read));
    CHECK(request_with_control(bridge, 2, Mode::Write).status == Status::Queued);
    CHECK(request_with_control(bridge, 3, Mode::Read).status == Status::Queued);
    CHECK(bridge.pending_notifications().empty());
    CHECK(!bridge.admitted(bridge.incarnation(), 2, Mode::Write));
    bridge.complete_undispatched_for_test(bridge.incarnation(), 1);
    auto messages = bridge.pending_notifications();
    CHECK(messages.size() == 1);
    CHECK(messages[0].continuation_id == 2);
    CHECK(messages[0].status == Status::Ready);
    CHECK(!bridge.admitted(bridge.incarnation(), 2, Mode::Write)); // CONTROL not yet accepted
    CHECK(!bridge.acknowledge({bridge.incarnation(), 2, Status::Cancelled}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 2, Status::Ready}));
    CHECK(bridge.admitted(bridge.incarnation(), 2, Mode::Write));
    CHECK(bridge.pending_notifications().empty());
    bridge.complete_undispatched_for_test(bridge.incarnation(), 2);
    CHECK(bridge.pending_notifications().size() == 1);
    CHECK(bridge.pending_notifications().front().continuation_id == 3);
    CHECK(bridge.acknowledge({bridge.incarnation(), 3, Status::Ready}));
    CHECK(bridge.admitted(bridge.incarnation(), 3, Mode::Read));
    bridge.complete_undispatched_for_test(bridge.incarnation(), 3);

    // Cancelling a queued writer publishes one durable terminal receipt.
    CHECK(request_with_control(bridge, 4, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(bridge, 5, Mode::Write).status == Status::Queued);
    CHECK(request_with_control(bridge, 6, Mode::Read).status == Status::Queued);
    bridge.cancel_pending(bridge.incarnation(), 5);
    messages = bridge.pending_notifications();
    CHECK(messages.size() == 2);
    CHECK(messages[0].continuation_id == 5 && messages[0].status == Status::Cancelled);
    CHECK(messages[1].continuation_id == 6 && messages[1].status == Status::Ready);
    CHECK(!bridge.acknowledge({bridge.incarnation(), 5, Status::Ready}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 5, Status::Cancelled}));
    CHECK(!bridge.acknowledge({bridge.incarnation(), 5, Status::Cancelled}));
    CHECK(bridge.acknowledge({bridge.incarnation(), 6, Status::Ready}));
    bridge.complete_undispatched_for_test(bridge.incarnation(), 4);
    bridge.complete_undispatched_for_test(bridge.incarnation(), 6);

    // READY-but-not-delivered tickets remain cancellable: waiting CONTROL
    // admission is not evidence a guest callback has begun.
    Bridge undelivered;
    CHECK(request_with_control(undelivered, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(undelivered, 2, Mode::Write).status == Status::Queued);
    CHECK(request_with_control(undelivered, 3, Mode::Read).status == Status::Queued);
    undelivered.complete_undispatched_for_test(undelivered.incarnation(), 1);
    CHECK(undelivered.pending_notifications().size() == 1);
    CHECK(!undelivered.admitted(undelivered.incarnation(), 2, Mode::Write));
    undelivered.cancel_pending(undelivered.incarnation(), 2);
    auto pending_cancel = undelivered.pending_notifications();
    CHECK(pending_cancel.size() == 2);
    CHECK(pending_cancel[0].continuation_id == 2
          && pending_cancel[0].status == Status::Cancelled);
    CHECK(pending_cancel[1].continuation_id == 3
          && pending_cancel[1].status == Status::Ready);
    CHECK(undelivered.acknowledge(pending_cancel[0]));
    CHECK(undelivered.acknowledge(pending_cancel[1]));
    CHECK(undelivered.admitted(undelivered.incarnation(), 3, Mode::Read));
    bool active_rejected = false;
    try { undelivered.cancel_pending(undelivered.incarnation(), 3); }
    catch (const std::logic_error&) { active_rejected = true; }
    CHECK(active_rejected);
    undelivered.complete_undispatched_for_test(undelivered.incarnation(), 3);

    // Cancellation races a queued wake: stale receipts cannot authorize work.
    CHECK(request_with_control(bridge, 7, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(bridge, 8, Mode::Write).status == Status::Queued);
    bridge.complete_undispatched_for_test(bridge.incarnation(), 7);
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
    try { (void)request_with_control(bridge, 9, Mode::Read); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);

    // A one-way continuation ID avoids stale CONTROL receipt ABA.
    Bridge monotonic;
    CHECK(request_with_control(monotonic, 100, Mode::Read).status == Status::Ready);
    monotonic.complete_undispatched_for_test(monotonic.incarnation(), 100);
    rejected = false;
    try { (void)request_with_control(monotonic, 100, Mode::Read); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);

    // Cancellation backpressure must not create unbounded retained
    // continuation records while the CONTROL channel is unavailable.
    Bridge bounded;
    CHECK(request_with_control(bounded, 1, Mode::Read).status == Status::Ready);
    for (std::uint64_t id = 2;
         id <= Bridge::max_outstanding_continuations; ++id) {
        CHECK(request_with_control(bounded, id, Mode::Write).status == Status::Queued);
        bounded.cancel_pending(bounded.incarnation(), id);
    }
    CHECK(bounded.pending_notifications().size() ==
          Bridge::max_outstanding_continuations - 1);
    CHECK(bounded.pending_notifications(0).empty());
    const auto small_batch = bounded.pending_notifications(3);
    CHECK(small_batch.size() == 3);
    CHECK(small_batch[0].continuation_id == 2);
    CHECK(small_batch[1].continuation_id == 3);
    CHECK(small_batch[2].continuation_id == 4);
    bool saturated = false;
    try {
        (void)request_with_control(bounded, Bridge::max_outstanding_continuations + 1,
                              Mode::Write);
    } catch (const std::overflow_error&) {
        saturated = true;
    }
    CHECK(saturated);
    for (std::uint64_t id = 2;
         id <= Bridge::max_outstanding_continuations; ++id) {
        CHECK(bounded.acknowledge({bounded.incarnation(), id, Status::Cancelled}));
    }
    CHECK(request_with_control(bounded, Bridge::max_outstanding_continuations + 1,
                          Mode::Write).status == Status::Queued);
    bounded.retire();
    CHECK(bounded.pending_notifications().size() == 2);

    // Retirement/reincarnation ABA: the old CONTROL receipt must not
    // admit a continuation with the same numerical ID in a replacement actor.
    Bridge previous;
    CHECK(request_with_control(previous, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(previous, 2, Mode::Write).status == Status::Queued);
    previous.complete_undispatched_for_test(previous.incarnation(), 1);
    const auto stale_ready = previous.pending_notifications().front();

    Bridge replacement;
    CHECK(previous.incarnation() != replacement.incarnation());
    CHECK(request_with_control(replacement, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(replacement, 2, Mode::Write).status == Status::Queued);
    replacement.complete_undispatched_for_test(replacement.incarnation(), 1);
    const auto current_ready = replacement.pending_notifications().front();
    CHECK(stale_ready.continuation_id == current_ready.continuation_id);
    CHECK(stale_ready.status == current_ready.status);
    CHECK(stale_ready.incarnation != current_ready.incarnation);
    CHECK(!replacement.acknowledge(stale_ready));
    CHECK(!replacement.admitted(previous.incarnation(), 2, Mode::Write));
    CHECK(!replacement.admitted(replacement.incarnation(), 2, Mode::Write));
    bool stale_identity_rejected = false;
    try { replacement.complete_undispatched_for_test(previous.incarnation(), 2); }
    catch (const std::invalid_argument&) { stale_identity_rejected = true; }
    CHECK(stale_identity_rejected);
    stale_identity_rejected = false;
    try { (void)replacement.request(previous.incarnation(), 3, Mode::Read); }
    catch (const std::invalid_argument&) { stale_identity_rejected = true; }
    CHECK(stale_identity_rejected);
    CHECK(replacement.acknowledge(current_ready));
    CHECK(replacement.admitted(replacement.incarnation(), 2, Mode::Write));
    replacement.complete_undispatched_for_test(replacement.incarnation(), 2);

    // Publication observation is retryable and side-effect free.
    Bridge retryable;
    CHECK(request_with_control(retryable, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(retryable, 2, Mode::Write).status == Status::Queued);
    retryable.complete_undispatched_for_test(retryable.incarnation(), 1);
    for (int i = 0; i < 20; ++i) {
        CHECK(retryable.pending_notifications().size() == 1);
        CHECK(!retryable.admitted(retryable.incarnation(), 2, Mode::Write));
    }
    CHECK(retryable.acknowledge({retryable.incarnation(), 2, Status::Ready}));
    CHECK(retryable.admitted(retryable.incarnation(), 2, Mode::Write));
    retryable.complete_undispatched_for_test(retryable.incarnation(), 2);
    return 0;
}
