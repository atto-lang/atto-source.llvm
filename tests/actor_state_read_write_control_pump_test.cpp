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

int main() {
    Registry registry;
    Pump pump(registry);
    CHECK(registry.request(1, Mode::Read).status == Status::Ready);
    CHECK(registry.request(2, Mode::Write).status == Status::Queued);
    CHECK(registry.request(3, Mode::Read).status == Status::Queued);
    registry.complete(1);

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
    CHECK(registry.admitted(2, Mode::Write));
    registry.complete(2);
    CHECK(registry.pending_notifications().size() == 1);
    CHECK(pump.pump(try_admit) == 0); // saturation does not lose event
    admitted = 0;
    CHECK(pump.pump(try_admit) == 1);
    CHECK(control[0].continuation_id == 3);
    CHECK(registry.admitted(3, Mode::Read));

    // Retirement revokes an already queued READY receipt as well.
    registry.retire();
    CHECK(!registry.admitted(3, Mode::Read));
    CHECK(!registry.admitted(2, Mode::Write));
    CHECK(registry.pending_notifications().size() == 1);
    CHECK(pump.pump(try_admit) == 0);
    admitted = 0;
    CHECK(pump.pump(try_admit) == 1);
    CHECK(control[0].continuation_id == 3);
    CHECK(control[0].status == Status::Cancelled);
    CHECK(registry.pending_notifications().empty());

    // Concurrent pumpers cannot acknowledge the same terminal event twice.
    Registry concurrency;
    Pump concurrent_pump(concurrency);
    CHECK(concurrency.request(10, Mode::Read).status == Status::Ready);
    CHECK(concurrency.request(11, Mode::Write).status == Status::Queued);
    CHECK(concurrency.request(12, Mode::Read).status == Status::Queued);
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
