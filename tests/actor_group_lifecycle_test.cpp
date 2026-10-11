#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeActorRef;
using namespace std::chrono_literals;

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
        ref.get_ready().get();
        check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
              "ready must not imply done");
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
        bool failed_done = false;
        try { next.get_done().get(); }
        catch (const std::runtime_error&) { failed_done = true; }
        check(failed_done, "startup failure must fail done on physical retirement");
        group.close();
        check(stop_count == 1, "group close must stop owned worker exactly once");
        check(group.worker_count() == 0, "closed group retained strong worker refs");
        check(!group.mark_ready(ref), "closed group resurrected startup");
        check(group.closed(), "group is not closed");
        check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
              "group close fabricated actor completion");
        check(group.finalized(ref), "closed group rejected trusted terminal acknowledgment");
        ref.get_done().get();
        check(!group.finalized(ref), "duplicate physical finalization accepted");
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
        check(!group.finalized(first), "old generation terminated replacement");
        check(group.finalized(replacement), "failed to retire replacement");
        replacement.get_done().get();
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
        check(first.get_done().wait_for(0ms) == std::future_status::timeout,
              "stop fabricated first done");
        check(second.get_done().wait_for(0ms) == std::future_status::timeout,
              "stop fabricated second done");
        check(group.finalized(first, "stopped"), "first terminal acknowledgment rejected");
        check(group.finalized(second, "stopped"), "second terminal acknowledgment rejected");
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
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "close/start race fabricated terminal done");
            check(group.finalized(ref), "close/start race lost terminal ack");
            bool failed_start = false;
            try { ref.get_ready().get(); }
            catch (const std::runtime_error&) { failed_start = true; }
            bool failed_done = false;
            try { ref.get_done().get(); }
            catch (const std::runtime_error&) { failed_done = true; }
            check(failed_start == failed_done,
                  "close/start race lost terminal failure correspondence");
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
        check(group.finalized(first), "first throwing worker not finalized");
        check(group.finalized(second), "second throwing worker not finalized");
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
        check(group.finalized(first), "reentrant first failed terminal ack");
        check(group.finalized(second), "reentrant second failed terminal ack");
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
    {
        // Different groups cannot claim the same live actor, even when the
        // first group has requested stop and detached its worker table.
        NativeActorGroup primary(991, 1), secondary(991, 1);
        auto old = primary.adopt(991, 14, 10, [] {});
        bool duplicate_rejected = false;
        try { (void) secondary.adopt(991, 14, 11, [] {}); }
        catch (const std::logic_error&) { duplicate_rejected = true; }
        check(duplicate_rejected, "cross-group active owner was stolen");
        primary.close();
        check(old.get_done().wait_for(0ms) == std::future_status::timeout,
              "cross-group close fabricated done");
        duplicate_rejected = false;
        try { (void) secondary.adopt(991, 14, 11, [] {}); }
        catch (const std::logic_error&) { duplicate_rejected = true; }
        check(duplicate_rejected, "cross-group owner stolen after close");
        check(primary.finalized(old), "closed owner unable to acknowledge teardown");
        auto current = secondary.adopt(991, 14, 11, [] {});
        check(!primary.finalized(old), "stale terminal ack accepted");
        check(!primary.mark_ready(current), "foreign supervisor admitted start");
        check(secondary.mark_ready(current), "new owner lost admission");
        check(secondary.finalized(current), "new owner failed physical ack");
        current.get_ready().get();
        current.get_done().get();
    }
    {
        // Stop, close, and late on_start failure must each settle the two
        // lifecycle futures exactly once, without pretending stop is done.
        for (int round = 0; round < 200; ++round) {
            NativeActorGroup group(992, 1);
            std::atomic<int> stops{0};
            auto ref = group.adopt(992, 22, static_cast<std::uint64_t>(round + 1),
                                   [&] { ++stops; });
            std::thread stopper([&] { group.stop_workers(); });
            std::thread closer([&] { group.close(); });
            stopper.join();
            closer.join();
            check(stops == 1, "concurrent stop/close duplicated stop callback");
            check(ref.get_done().wait_for(0ms) == std::future_status::timeout,
                  "stop and close fabricated finalization");
            check(group.finalized(ref, "startup failed"),
                  "late startup failure rejected after close");
            check(!group.finalized(ref, "duplicated"),
                  "duplicate startup failure acknowledged");
            check(ref.get_ready().wait_for(0ms) == std::future_status::ready,
                  "startup failure stranded ready future");
            check(ref.get_done().wait_for(0ms) == std::future_status::ready,
                  "startup failure stranded done future");
            bool ready_failed = false, done_failed = false;
            try { ref.get_ready().get(); }
            catch (const std::runtime_error&) { ready_failed = true; }
            try { ref.get_done().get(); }
            catch (const std::runtime_error&) { done_failed = true; }
            check(ready_failed && done_failed,
                  "terminal startup error was not propagated to both observers");
        }
    }
    {
        // Physical-finalization acknowledgment racing with close does not
        // resurrect a worker or duplicate terminal promise completion.
        for (int round = 0; round < 200; ++round) {
            NativeActorGroup group(993, 1);
            auto ref = group.adopt(993, 99, static_cast<std::uint64_t>(round + 1),
                                   [] {});
            std::thread closed([&] { group.close(); });
            std::thread finished([&] {
                (void) group.finalized(ref, "initialization failed");
            });
            closed.join();
            finished.join();
            check(ref.get_ready().wait_for(0ms) == std::future_status::ready,
                  "close/finalize race left readiness unresolved");
            check(ref.get_done().wait_for(0ms) == std::future_status::ready,
                  "close/finalize race left done unresolved");
            check(!group.finalized(ref), "close/finalize race admitted duplicate");
        }
    }
    {
        // A failure before on_start completion has a failed done, even when
        // a host finalizer reports physical quiescence without extra error.
        NativeActorGroup group(994, 1);
        auto ref = group.adopt(994, 3, 1, [] {});
        check(group.finalized(ref), "pre-ready physical acknowledgment missing");
        bool ready_failed = false, done_failed = false;
        try { ref.get_ready().get(); }
        catch (const std::runtime_error&) { ready_failed = true; }
        try { ref.get_done().get(); }
        catch (const std::runtime_error&) { done_failed = true; }
        check(ready_failed && done_failed,
              "uninitialized actor must fail both ready and done");
    }

    {
        // The actual current-main NativeActorGroup owns the reserved CONTROL
        // lane; GPU producer callbacks can enqueue but cannot run user code.
        NativeActorGroup group(1201, 2);
        auto a = group.adopt(1201, 1, 1, [] {});
        auto b = group.adopt(1201, 2, 1, [] {});
        check(group.mark_ready(a) && group.mark_ready(b), "CONTROL workers not ready");
        std::atomic<int> executed{0};
        std::thread producer([&] {
            check(group.enqueue_control(a, [&] { ++executed; }), "producer CONTROL enqueue failed");
        });
        producer.join();
        check(executed == 0, "producer ran actor handler on callback thread");
        check(group.has_pending_control(a), "reserved CONTROL slot missing");
        check(!group.enqueue_control(a, [] {}), "CONTROL slot accepted over capacity");
        check(!group.finalized(a), "actor finalized with reserved CONTROL");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::executed,
              "scheduler did not admit CONTROL");
        check(executed == 1, "actor callback did not run on scheduler turn");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::idle,
              "CONTROL queue not cleared");
        check(group.finalized(a) && group.finalized(b), "CONTROL actors not finalized");
        a.get_done().get(); b.get_done().get();
    }
    {
        NativeActorGroup group(1202, 2);
        auto a = group.adopt(1202, 1, 1, [] {});
        auto b = group.adopt(1202, 2, 1, [] {});
        check(group.mark_ready(a) && group.mark_ready(b), "CONTROL workers not ready");
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        check(group.enqueue_control(a, [&] {
            entered.set_value();
            gate.wait();
        }), "unable to enqueue controlled long turn");
        check(group.enqueue_control(b, [] {}), "unable to enqueue second actor CONTROL");
        std::thread carrier([&] {
            check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::executed,
                  "first carrier rejected CONTROL turn");
        });
        entered.get_future().wait();
        // Separate actors have separate logical lanes and may execute
        // CONTROL concurrently on distinct physical carriers.
        check(group.run_control_turn(b) == NativeActorGroup::ControlTurn::executed,
              "unrelated actor CONTROL was blocked by busy actor");
        check(!group.finalized(a), "active CONTROL bypassed physical finalization gate");
        release.set_value();
        carrier.join();
        check(group.run_control_turn(b) == NativeActorGroup::ControlTurn::idle,
              "independent CONTROL delivered more than once");
        check(group.finalized(a) && group.finalized(b), "CONTROL not quiescent");
    }
    {
        NativeActorGroup group(1203, 1);
        auto a = group.adopt(1203, 1, 1, [] {});
        check(group.mark_ready(a), "CONTROL close actor not ready");
        check(group.enqueue_control(a, [] {}), "CONTROL could not reserve");
        group.close();
        check(a.control_delivery_lost(), "close silently dropped terminal CONTROL");
        check(group.finalized(a), "closed group could not retire discarded CONTROL");
        a.get_done().get();
    }

}
