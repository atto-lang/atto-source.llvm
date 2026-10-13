#define ORESLANG_RW_INTERNAL_TEST_HOOKS 1
#include "oreslang/runtime/ActorStateReadWriteControlPump.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using Registry = oreslang::runtime::ActorStateReadWriteContinuations;
using Pump = oreslang::runtime::ActorStateReadWriteControlPump;
using Status = Registry::Status;
using Mode = Registry::Mode;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

// Simulate an accepted bounded CONTROL receipt for the initial grant.
static Registry::Request request_with_control(Registry& registry,
                                               std::uint64_t id, Mode mode) {
    const auto grant = registry.request(registry.incarnation(), id, mode);
    if (grant.status == Status::Ready) {
        CHECK(!registry.admitted(registry.incarnation(), id, mode));
        CHECK(registry.acknowledge({registry.incarnation(), id, Status::Ready}));
    }
    return grant;
}

int main() {
    // No contended or uncontended ticket may authorize a guest operation
    // without a real CONTROL admission callback accepting its notification.
    Registry initial;
    Pump initial_pump(initial);
    CHECK(initial.request(initial.incarnation(), 1, Mode::Read).status == Status::Ready);
    CHECK(!initial.admitted(initial.incarnation(), 1, Mode::Read));
    CHECK(initial.pending_notifications().size() == 1);
    auto first_control_receipt =
            [](const Registry::Notification&) noexcept -> bool { return true; };
    CHECK(initial_pump.pump(first_control_receipt, 0) == 0);
    CHECK(!initial.admitted(initial.incarnation(), 1, Mode::Read));
    CHECK(initial_pump.pump(first_control_receipt) == 1);
    CHECK(initial.admitted(initial.incarnation(), 1, Mode::Read));
    initial.complete_undispatched_for_test(initial.incarnation(), 1);

    Registry registry;
    Pump pump(registry);
    CHECK(request_with_control(registry, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(registry, 2, Mode::Write).status == Status::Queued);
    CHECK(request_with_control(registry, 3, Mode::Read).status == Status::Queued);
    registry.complete_undispatched_for_test(registry.incarnation(), 1);

    std::array<Registry::Notification, 1> control{};
    std::size_t admitted = 0;
    auto try_admit = [&](const Registry::Notification& notification) noexcept -> bool {
        if (admitted == control.size()) return false;
        control[admitted++] = notification;
        return true;
    };

    CHECK(pump.pump(try_admit) == 1);
    CHECK(admitted == 1 && control[0].continuation_id == 2);
    CHECK(control[0].status == Status::Ready);
    CHECK(pump.pump(try_admit) == 0); // no duplicates after receipt
    CHECK(registry.admitted(registry.incarnation(), 2, Mode::Write));
    registry.complete_undispatched_for_test(registry.incarnation(), 2);
    CHECK(registry.pending_notifications().size() == 1);
    CHECK(pump.pump(try_admit) == 0); // saturation does not lose event
    admitted = 0;
    CHECK(pump.pump(try_admit) == 1);
    CHECK(control[0].continuation_id == 3);
    CHECK(registry.admitted(registry.incarnation(), 3, Mode::Read));

    // Retirement revokes an already queued READY receipt as well.
    registry.retire();
    CHECK(!registry.admitted(registry.incarnation(), 3, Mode::Read));
    CHECK(!registry.admitted(registry.incarnation(), 2, Mode::Write));
    CHECK(registry.pending_notifications().size() == 1);
    CHECK(pump.pump(try_admit) == 0);
    admitted = 0;
    CHECK(pump.pump(try_admit) == 1);
    CHECK(control[0].continuation_id == 3);
    CHECK(control[0].status == Status::Cancelled);
    CHECK(registry.pending_notifications().empty());

    // A trusted callback may accidentally reenter the pump. It must
    // fail promptly, not deadlock the only actor scheduler carrier.
    Registry recursive;
    Pump recursive_pump(recursive);
    CHECK(request_with_control(recursive, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(recursive, 2, Mode::Write).status == Status::Queued);
    recursive.complete_undispatched_for_test(recursive.incarnation(), 1);
    int nested_receipts = -1;
    auto reject_nested = [](const Registry::Notification&) noexcept -> bool {
        return false;
    };
    auto reenter = [&](const Registry::Notification&) noexcept -> bool {
        nested_receipts = static_cast<int>(recursive_pump.pump(reject_nested));
        return false;
    };
    CHECK(recursive_pump.pump(reenter) == 0);
    CHECK(nested_receipts == 0);
    CHECK(recursive.pending_notifications().size() == 1);

    // CONTROL admission can race actor retirement after the recipient
    // queues READY. Its receipt must not authorize stale guest execution.
    Registry retiring;
    Pump retiring_pump(retiring);
    CHECK(request_with_control(retiring, 1, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(retiring, 2, Mode::Write).status == Status::Queued);
    retiring.complete_undispatched_for_test(retiring.incarnation(), 1);
    Registry::Notification queued_ready{};
    auto race_retire = [&](const Registry::Notification& n) noexcept -> bool {
        queued_ready = n;
        retiring.retire();
        return true;
    };
    CHECK(retiring_pump.pump(race_retire) == 1);
    CHECK(queued_ready.status == Status::Ready);
    CHECK(!retiring.admitted(queued_ready.incarnation,
                             queued_ready.continuation_id, Mode::Write));
    CHECK(retiring.pending_notifications().size() == 1);
    CHECK(retiring.pending_notifications().front().status == Status::Cancelled);
    auto admit_cancel = [](const Registry::Notification&) noexcept -> bool { return true; };
    CHECK(retiring_pump.pump(admit_cancel) == 1);
    CHECK(retiring.pending_notifications().empty());

    // Concurrent pumpers cannot acknowledge the same terminal event twice.
    Registry concurrency;
    Pump concurrent_pump(concurrency);
    CHECK(request_with_control(concurrency, 10, Mode::Read).status == Status::Ready);
    CHECK(request_with_control(concurrency, 11, Mode::Write).status == Status::Queued);
    CHECK(request_with_control(concurrency, 12, Mode::Read).status == Status::Queued);
    concurrency.retire();
    std::array<Registry::Notification, 1> one_slot{};
    std::size_t occupied = 0;
    auto one_try = [&](const Registry::Notification& n) noexcept -> bool {
        if (occupied == 1) return false;
        one_slot[occupied++] = n;
        return true;
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&] { (void)concurrent_pump.pump(one_try); });
    }
    for (auto& worker : workers) worker.join();
    CHECK(occupied == 1);
    CHECK(concurrency.pending_notifications().size() == 2);

    occupied = 0;
    CHECK(concurrent_pump.pump(one_try, 1) == 1);
    CHECK(occupied == 1);
    occupied = 0;
    CHECK(concurrent_pump.pump(one_try, 1) == 1);
    CHECK(concurrency.pending_notifications().empty());
    return 0;
}
