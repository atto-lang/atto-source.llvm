#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "oreslang/runtime/ActorIsolationAccessV1.hpp"

using atto::security::ActorIsolationAccessV1;
using namespace std::chrono_literals;

int main() {
    ActorIsolationAccessV1 owner(2, 3, 4, 128);
    auto allocation = owner.issue_allocation(8, 24);
    assert(allocation.has_value());
    std::atomic<int> calls{0};
    assert(owner.with_checked_access(*allocation, 2, 8, [&](std::uint64_t o) {
        assert(o == 8); ++calls;
    }));
    assert(calls == 1);
    // Invalid/stale/foreign range/ownership cannot enter callback.
    assert(!owner.with_checked_access(*allocation, 1, 8, [&](auto) { ++calls; }));
    assert(!owner.with_checked_access(*allocation, 4, 9, [&](auto) { ++calls; }));
    assert(!owner.with_checked_access(*allocation, 0, 8, [&](auto) { ++calls; }));
    assert(!owner.with_checked_access(*allocation, 2, UINT64_MAX, [&](auto) { ++calls; }));
    ActorIsolationAccessV1 sibling(2, 5, 4, 128);
    assert(!sibling.with_checked_access(*allocation, 2, 8, [&](auto) { ++calls; }));
    assert(calls == 1);

    // A throwing callback must release the gate before later admission.
    bool caught = false;
    try {
        owner.with_checked_access(*allocation, 2, 8, [](auto) {
            throw std::runtime_error("controlled failure");
        });
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    assert(owner.with_checked_access(*allocation, 2, 8, [&](auto) { ++calls; }));
    assert(calls == 2);

    // The callback remains active in one task while another task attempts a
    // release. Since gate_ is held, the release cannot complete until exit.
    std::mutex barrier_gate;
    std::condition_variable cv;
    bool active = false;
    bool finish = false;
    auto access_future = std::async(std::launch::async, [&] {
        return owner.with_checked_access(*allocation, 2, 8, [&](std::uint64_t o) {
            assert(o == 8);
            std::unique_lock<std::mutex> lock(barrier_gate);
            active = true;
            cv.notify_all();
            cv.wait(lock, [&] { return finish; });
        });
    });
    {
        std::unique_lock<std::mutex> lock(barrier_gate);
        cv.wait(lock, [&] { return active; });
    }
    std::promise<void> attempting;
    auto attempting_future = attempting.get_future();
    auto release_future = std::async(std::launch::async, [&] {
        attempting.set_value();
        return owner.release(*allocation);
    });
    attempting_future.wait();
    assert(release_future.wait_for(40ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(barrier_gate);
        finish = true;
    }
    cv.notify_all();
    assert(access_future.get());
    assert(release_future.get());
    assert(!owner.with_checked_access(*allocation, 2, 8, [&](auto) { ++calls; }));
    assert(calls == 2);

    auto next = owner.issue_allocation(8, 24);
    assert(next);
    assert(!owner.with_checked_access(*allocation, 2, 8, [&](auto) { ++calls; }));
    assert(owner.with_checked_access(*next, 3, 16, [&](auto offset) {
        assert(offset == 16); ++calls;
    }));
    assert(calls == 3);

    // Retirement cannot interleave with an admitted synchronous memory access.
    std::promise<void> may_retire;
    auto may_retire_future = may_retire.get_future();
    std::promise<void> done_access;
    auto done_access_future = done_access.get_future();
    auto holder = std::async(std::launch::async, [&] {
        return owner.with_checked_access(*next, 2, 8, [&](auto) {
            may_retire.set_value();
            done_access_future.wait();
        });
    });
    may_retire_future.wait();
    auto retirement = std::async(std::launch::async, [&] { owner.retire(); });
    assert(retirement.wait_for(40ms) == std::future_status::timeout);
    done_access.set_value();
    assert(holder.get());
    retirement.get();
    assert(owner.retired());
    assert(!owner.with_checked_access(*next, 2, 8, [&](auto) { ++calls; }));
    assert(!owner.issue_allocation(8, 8));

    // Actual host-managed bytes: scrub happens before the range becomes
    // eligible for reissue, and the scoped grant is revoked afterward.
    ActorIsolationAccessV1 ledger(20, 30, 40, 64);
    auto secret = ledger.issue_allocation(8, 16);
    assert(secret);
    std::array<unsigned char, 64> arena{};
    arena[8] = 0x5a;
    arena[16] = 0xa5;
    assert(ledger.release_after_scrub(*secret, [&](std::uint64_t start,
                                                     std::uint64_t len) {
        assert(start == 8 && len == 16);
        for (std::uint64_t i = start; i < start + len; ++i)
            arena[static_cast<std::size_t>(i)] = 0;
    }));
    assert(arena[8] == 0 && arena[16] == 0);
    assert(!ledger.with_checked_access(*secret, 2, 8, [&](auto) { ++calls; }));
    auto replacement = ledger.issue_allocation(8, 16);
    assert(replacement);
    assert(!ledger.release_after_scrub(*secret, [&](auto, auto) { ++calls; }));
    assert(calls == 3);
    bool scrub_threw = false;
    try {
        ledger.release_after_scrub(*replacement, [&](auto, auto) {
            throw std::runtime_error("scrub failed");
        });
    } catch (const std::runtime_error&) { scrub_threw = true; }
    assert(scrub_threw);
    assert(ledger.retired()); // partial zeroization quarantines the incarnation
    assert(!ledger.with_checked_access(*replacement, 2, 8, [&](auto) { ++calls; }));
    assert(!ledger.release_after_scrub(*replacement, [](auto, auto) {}));
    assert(!ledger.issue_allocation(8, 16));
}
