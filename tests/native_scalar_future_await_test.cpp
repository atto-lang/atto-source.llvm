#include "oreslang/runtime/NativeScalarFutureAwait.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeScalarFutureAwait;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    NativeActorGroup group(9701, 2);
    auto owner = group.adopt(9701, 1, 1, [] {});
    auto foreign = group.adopt(9701, 2, 1, [] {});
    check(group.mark_ready(owner) && group.mark_ready(foreign), "admission failed");
    auto future = NativeScalarFutureAwait::open(owner, 17);
    const auto id = future->id();
    auto stale = id;
    stale.generation += 1;
    check(!future->complete(stale, 1), "stale generation settled value");
    check(!future->fail(id, 0), "invalid failure status admitted");
    check(!future->try_schedule(id, group, owner, [] (const auto&) {}),
          "pending future scheduled");
    check(group.enqueue_control(owner, [] {}), "unable to fill CONTROL");
    std::thread producer([future, id] {
        check(future->complete(id, 42), "completion lost");
        check(!future->complete(id, 43), "duplicate completion admitted");
    });
    producer.join();
    int resumed = 0;
    auto resume = [&](const NativeScalarFutureAwait::Result& value) {
        check(value.success && value.value == 42 && value.error_code == 0,
              "corrupt scalar future result");
        ++resumed;
    };
    check(!future->try_schedule(id, group, foreign, resume), "foreign CONTROL owner");
    check(!future->try_schedule(id, group, owner, resume), "full CONTROL accepted");
    check(future->phase() == NativeScalarFutureAwait::Phase::ready,
          "backpressure erased terminal value");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "failed to drain CONTROL");
    check(future->try_schedule(id, group, owner, resume), "retry lost value");
    check(resumed == 0, "guest resumed inline on scheduling thread");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "CONTROL did not resume");
    check(resumed == 1, "not exactly one resumption");
    check(future->phase() == NativeScalarFutureAwait::Phase::completed,
          "frame not completed");
    check(!future->try_schedule(id, group, owner, resume), "repeated resume accepted");
    check(!future->cancel(id), "completed resume cancelled");

    // Cancellation after CONTROL admission revokes queued guest invocation.
    auto queued = NativeScalarFutureAwait::open(owner, 18);
    check(queued->fail(queued->id(), 7), "failure not recorded");
    check(queued->try_schedule(queued->id(), group, owner,
          [&](const auto&) { ++resumed; }), "could not enqueue failed result");
    check(queued->cancel(queued->id()), "queued cancel rejected");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "cancelled CONTROL not drained");
    check(resumed == 1, "cancelled guest callback executed");

    // A failed value must be delivered (not treated as cancellation), and
    // a future ID from the same actor must not authorize an unrelated frame.
    auto failed = NativeScalarFutureAwait::open(owner, 19);
    auto other_ticket = failed->id();
    other_ticket.future_id += 1;
    check(!failed->fail(other_ticket, 17), "wrong future ID settled failure");
    check(failed->fail(failed->id(), 17), "failure not settled");
    int reported_failure = 0;
    check(failed->try_schedule(failed->id(), group, owner,
        [&](const NativeScalarFutureAwait::Result& outcome) {
            check(!outcome.success && outcome.value == 0 &&
                  outcome.error_code == 17, "failure result was mangled");
            ++reported_failure;
        }), "failure CONTROL rejected");
    check(reported_failure == 0, "failure callback executed inline");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "failure CONTROL did not run");
    check(reported_failure == 1, "failure result not delivered once");
    check(!failed->complete(failed->id(), 42), "failed future resurrected");

    // Three contenders race. Exactly one value (success or failure) can
    // settle, unless cancellation got there first. Cancelling a settled but
    // undelivered value is the documented revocation (see the queued case
    // above), not a second settlement: it must leave no callback behind.
    for (std::uint64_t n = 1; n <= 256; ++n) {
        auto raced = NativeScalarFutureAwait::open(owner, 100 + n);
        const auto ticket = raced->id();
        std::atomic<int> settled{0};
        std::atomic<bool> cancelled{false};
        std::thread a([&] { if (raced->complete(ticket, 5)) ++settled; });
        std::thread b([&] { if (raced->fail(ticket, 9)) ++settled; });
        std::thread c([&] { if (raced->cancel(ticket)) cancelled = true; });
        a.join(); b.join(); c.join();
        check(settled <= 1, "terminal value settled twice");
        check(settled == 1 || cancelled, "race ended with no terminal winner");
        check(!raced->cancel(ticket), "cancelled twice");
        check(!raced->complete(ticket, 6) && !raced->fail(ticket, 10),
              "terminal frame accepted a late value");
        if (raced->phase() == NativeScalarFutureAwait::Phase::ready) {
            int callbacks = 0;
            check(raced->try_schedule(ticket, group, owner,
                [&](const auto&) { ++callbacks; }), "settled race not scheduled");
            check(group.run_control_turn(owner)
                  == NativeActorGroup::ControlTurn::executed, "CONTROL race lost");
            check(callbacks == 1, "raced future callback count");
        } else {
            check(raced->phase() == NativeScalarFutureAwait::Phase::cancelled,
                  "race ended in invalid state");
        }
    }
    check(group.finalized(owner) && group.finalized(foreign), "group not quiescent");
    return 0;
}
