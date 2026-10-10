#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeActorRef;

static void check(bool yes, const char* error) {
    if (!yes) throw std::runtime_error(error);
}

int main() {
    {
        NativeActorGroup group(100, 2);
        int stop_count = 0;
        NativeActorRef ref = group.adopt(100, 7, 1, [&] { ++stop_count; });
        int callbacks = 0;
        ref.when_ready([&](const auto& result) {
            check(result.ready, "ready should succeed");
            ++callbacks;
        });
        check(callbacks == 0, "unawaited spawn must not wait for startup");
        check(group.worker_count() == 1, "group does not own worker ref");
        check(group.mark_ready(ref), "ready failed to publish");
        ref.when_ready([&](const auto& result) {
            check(result.ready, "already settled readiness changed");
            ++callbacks;
        });
        check(callbacks == 2, "pending/late awaits not both fulfilled");
        check(!group.mark_ready(ref), "duplicate startup should be rejected");

        bool full = false;
        NativeActorRef next = group.adopt(100, 8, 1, [] {});
        try { (void) group.adopt(100, 9, 1, [] {}); }
        catch (const std::logic_error&) { full = true; }
        check(full, "worker capacity not enforced");
        check(group.retire(next, "initializer failed"), "startup failure not retired");
        int late_failure = 0;
        next.when_ready([&](const auto& result) {
            check(!result.ready && result.error == "initializer failed",
                  "late await must see original init failure");
            ++late_failure;
        });
        check(late_failure == 1, "failed actor readiness ticket lost on retirement");
        group.close();
        check(stop_count == 1, "group close must stop owned worker exactly once");
        check(group.worker_count() == 0, "closed group retained strong worker refs");
        check(!group.mark_ready(ref), "closed group resurrected startup");
        check(group.closed(), "group is not closed");
    }
    {
        NativeActorGroup group(1, 1);
        bool rejected_foreign = false;
        try { (void) group.adopt(2, 4, 1, [] {}); }
        catch (const std::invalid_argument&) { rejected_foreign = true; }
        check(rejected_foreign, "foreign runtime actor admitted");
        auto first = group.adopt(1, 4, 10, [] {});
        check(group.retire(first, "gone"), "first generation not retired");
        auto replacement = group.adopt(1, 4, 11, [] {});
        check(!group.mark_ready(first), "stale generation mutated replacement");
        check(group.mark_ready(replacement), "new incarnation rejected");
        int callback_count = 0;
        first.when_ready([&](const auto& result) {
            check(!result.ready, "stale ticket was reused after actor ID reuse");
            ++callback_count;
        });
        replacement.when_ready([&](const auto& result) {
            check(result.ready, "new ticket not ready");
            ++callback_count;
        });
        check(callback_count == 2, "incarnation readiness contamination");
    }
    {
        NativeActorGroup group(1, 2);
        int stops = 0;
        auto first = group.adopt(1, 1, 1, [&] { ++stops; });
        auto second = group.adopt(1, 2, 1, [&] { ++stops; });
        int failure_count = 0;
        first.when_ready([&](const auto& value) {
            check(!value.ready, "stopped pre-start actor reported ready");
            ++failure_count;
        });
        second.when_ready([&](const auto& value) {
            check(!value.ready, "stopped pre-start actor reported ready");
            ++failure_count;
        });
        group.stop_workers();
        check(stops == 2, "group stop did not request both workers");
        check(failure_count == 2, "stopped workers left readiness hanging");
        group.close();
        check(stops == 2, "close double-stopped workers");
    }
    {
        // Publish readiness and group close simultaneously. Whichever wins
        // settles exactly once, with no callback under the group mutex.
        for (int round = 0; round < 100; ++round) {
            NativeActorGroup group(1, 1);
            std::atomic<int> stops{0};
            std::atomic<int> observed{0};
            auto ref = group.adopt(1, 42, static_cast<std::uint64_t>(round + 1),
                                   [&] { stops.fetch_add(1); });
            ref.when_ready([&](const auto&) { observed.fetch_add(1); });
            std::thread started([&] { (void) group.mark_ready(ref); });
            std::thread closed([&] { group.close(); });
            started.join();
            closed.join();
            check(observed.load() == 1, "close/start race lost or duplicated readiness");
            check(stops.load() == 1, "close/start race duplicated stop request");
            check(group.worker_count() == 0, "close/start race retained ref");
        }
    }
    {
        // Throwing callbacks must never prevent other callbacks or actor stop.
        NativeActorGroup group(1, 3);
        std::atomic<int> stops{0};
        auto first = group.adopt(1, 1, 1, [&] {
            stops.fetch_add(1);
            throw std::runtime_error("stop hook failed");
        });
        auto second = group.adopt(1, 2, 1, [&] { stops.fetch_add(1); });
        int observers = 0;
        first.when_ready([&](const auto&) {
            ++observers;
            throw std::runtime_error("observer failed");
        });
        second.when_ready([&](const auto& result) {
            check(!result.ready, "close accepted uninitialized worker");
            ++observers;
        });
        group.close();
        check(group.closed() && group.worker_count() == 0,
              "close failed to detach all references");
        check(stops.load() == 2, "one throwing stop hook hid another worker");
        check(observers == 2, "one throwing readiness callback hid another");
        group.close();
        check(stops.load() == 2, "second close requested duplicate stop");
    }
    {
        // Reentrant close from a stop hook cannot deadlock or double-stop.
        NativeActorGroup group(1, 2);
        int first_stops = 0, second_stops = 0;
        auto first = group.adopt(1, 1, 1, [&] {
            ++first_stops;
            group.close();
        });
        auto second = group.adopt(1, 2, 1, [&] { ++second_stops; });
        (void) first;
        (void) second;
        group.stop_workers();
        check(group.closed(), "reentrant close not processed");
        check(first_stops == 1 && second_stops == 1,
              "reentrant close duplicated or omitted stop request");
    }
    {
        // Callbacks may re-enter group methods; no group lock is held.
        NativeActorGroup group(1, 1);
        auto ref = group.adopt(1, 1, 1, [] {});
        bool reentered = false;
        ref.when_ready([&](const auto& result) {
            check(result.ready, "reentrant readiness must be success");
            reentered = group.retire(ref);
        });
        check(group.mark_ready(ref), "callback reentry blocked readiness");
        check(reentered && group.worker_count() == 0,
              "callback was invoked with ActorGroup mutex held");
    }
}
