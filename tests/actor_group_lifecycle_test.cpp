#include "oreslang/runtime/ActorGroup.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG // lifecycle regression assertions must run in Release builds
#endif
#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorGroup;
using namespace std::chrono_literals;

template <typename F>
static void require_throws(F&& f) {
    bool failed = false;
    try { f(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}

static void spawn_ref_is_pending_until_startup_and_done_is_independent() {
    ActorGroup group(2);
    std::atomic<int> stops{0};
    const auto ref = group.register_spawn([&] { stops.fetch_add(1); });
    assert(ref.group_id() == group.id());
    assert(ref.id() != 0);
    assert(ref.get_ready().wait_for(0ms) == std::future_status::timeout);
    assert(ref.get_done().wait_for(0ms) == std::future_status::timeout);
    assert(group.size() == 1);

    assert(group.mark_ready(ref));
    ref.get_ready().get();
    assert(ref.get_done().wait_for(0ms) == std::future_status::timeout);
    assert(!group.mark_ready(ref));
    assert(group.finalized(ref));
    ref.get_done().get();
    assert(!group.finalized(ref));
    assert(group.size() == 0);
    assert(stops == 0);
}

static void startup_failure_propagates_to_both_observers() {
    ActorGroup group(1);
    const auto ref = group.register_spawn([] {});
    assert(group.startup_failed(ref,
        std::make_exception_ptr(std::runtime_error("startup failed"))));
    require_throws([&] { ref.get_ready().get(); });
    require_throws([&] { ref.get_done().get(); });
    assert(!group.mark_ready(ref));
    assert(group.size() == 0);
}

static void cancelled_startup_failure_must_settle_done_exactly_once() {
    for (int i = 0; i < 200; ++i) {
        ActorGroup group(1);
        std::atomic<int> stops{0};
        const auto ref = group.register_spawn([&] { ++stops; });
        if ((i & 1) == 0) {
            group.close();
        } else {
            assert(group.request_stop(ref));
        }
        assert(stops == 1);
        // The cancellation observer is already settled, but physical cleanup
        // and the subsequent startup failure must still settle termination.
        require_throws([&] { ref.get_ready().get(); });
        assert(ref.get_done().wait_for(0ms) == std::future_status::timeout);
        assert(group.startup_failed(ref,
            std::make_exception_ptr(std::runtime_error("startup failed after stop"))));
        require_throws([&] { ref.get_done().get(); });
        assert(ref.get_done().wait_for(0ms) == std::future_status::ready);
        assert(group.size() == 0);
        assert(!group.startup_failed(ref,
            std::make_exception_ptr(std::runtime_error("duplicate"))));
        assert(!group.finalized(ref));
    }
}

static void concurrent_shutdown_and_failed_startup_do_not_strand_futures() {
    for (int i = 0; i < 200; ++i) {
        ActorGroup group(1);
        std::atomic<int> stops{0};
        const auto ref = group.register_spawn([&] { ++stops; });
        std::thread failure([&] {
            group.startup_failed(ref,
                std::make_exception_ptr(std::runtime_error("start failed")));
        });
        group.close();
        failure.join();
        assert(ref.get_ready().wait_for(0ms) == std::future_status::ready);
        assert(ref.get_done().wait_for(0ms) == std::future_status::ready);
        require_throws([&] { ref.get_ready().get(); });
        require_throws([&] { ref.get_done().get(); });
        assert(group.size() == 0);
        assert(stops <= 1);
    }
}

static void close_fences_admission_and_only_requests_stop() {
    ActorGroup group(2);
    std::atomic<int> stops{0};
    const auto unready = group.register_spawn([&] { ++stops; });
    const auto ready = group.register_spawn([&] { ++stops; });
    assert(group.mark_ready(ready));
    group.close();
    group.close();
    assert(group.closed());
    assert(stops == 2);
    require_throws([&] { unready.get_ready().get(); });
    ready.get_ready().get();
    assert(!group.mark_ready(unready));
    assert(unready.get_done().wait_for(0ms) == std::future_status::timeout);
    assert(group.size() == 2); // physical finalization not fabricated
    require_throws([&] { group.register_spawn([] {}); });
    assert(group.finalized(unready));
    assert(group.finalized(ready));
    unready.get_done().get();
    ready.get_done().get();
    assert(group.size() == 0);
}

static void stale_foreign_and_quota_cases_fail_closed() {
    ActorGroup first(1);
    ActorGroup second(1);
    auto a = first.register_spawn([] {});
    require_throws([&] { first.register_spawn([] {}); });
    assert(!second.mark_ready(a));
    assert(!second.finalized(a));
    assert(!second.request_stop(a));
    assert(first.mark_ready(a));
    assert(first.finalized(a));
    const auto b = first.register_spawn([] {});
    assert(b.id() != a.id()); // no ABA/reuse
    assert(!first.mark_ready(a));
    assert(first.mark_ready(b));
    assert(first.finalized(b));
    assert(first.size() == 0);
}

static void readiness_racing_close_cannot_resurrect_a_worker() {
    for (int i = 0; i < 200; ++i) {
        ActorGroup group(1);
        std::atomic<int> stops{0};
        auto ref = group.register_spawn([&] { ++stops; });
        std::thread initializer([&] { group.mark_ready(ref); });
        group.close();
        initializer.join();

        // The group may win (failed readiness) or on_start may win
        // (successful readiness), but the observer always settles exactly once.
        assert(ref.get_ready().wait_for(0ms) == std::future_status::ready);
        try { ref.get_ready().get(); } catch (const std::runtime_error&) {}
        assert(!group.mark_ready(ref));
        assert(stops == 1);
        assert(group.finalized(ref));
        ref.get_done().get();
    }
}

static void gpu_device_callbacks_only_post_to_reserved_control_lane() {
    ActorGroup group(2);
    auto worker = group.register_spawn([] {});
    auto other = group.register_spawn([] {});
    assert(group.mark_ready(worker) && group.mark_ready(other));
    std::atomic<int> actor_calls{0};
    std::thread device_callback([&] {
        assert(group.enqueue_control(worker, [&] { ++actor_calls; }));
    });
    device_callback.join();
    assert(actor_calls == 0); // provider did not execute actor handler
    assert(group.has_pending_control(worker));
    assert(!group.enqueue_control(worker, [] {})); // bounded slot
    assert(!group.finalized(worker)); // reserved event must be consumed
    assert(group.run_control_turn(worker) == ActorGroup::ControlTurn::executed);
    assert(actor_calls == 1);
    assert(!group.has_pending_control(worker));
    assert(group.run_control_turn(worker) == ActorGroup::ControlTurn::idle);
    assert(group.finalized(worker));
    assert(!group.enqueue_control(worker, [] {}));
    assert(group.run_control_turn(worker) == ActorGroup::ControlTurn::retired);
    assert(group.finalized(other));
}

static void group_control_lane_is_single_flight_even_on_different_carriers() {
    ActorGroup group(2);
    auto a = group.register_spawn([] {});
    auto b = group.register_spawn([] {});
    assert(group.mark_ready(a) && group.mark_ready(b));
    std::promise<void> entered, release;
    std::shared_future<void> unblock = release.get_future().share();
    std::atomic<int> simultaneous{0};
    assert(group.enqueue_control(a, [&] {
        ++simultaneous;
        entered.set_value();
        unblock.wait();
        --simultaneous;
    }));
    assert(group.enqueue_control(b, [&] {
        assert(simultaneous == 0);
        ++simultaneous;
        --simultaneous;
    }));
    std::thread carrier([&] {
        assert(group.run_control_turn(a) == ActorGroup::ControlTurn::executed);
    });
    entered.get_future().wait();
    assert(group.run_control_turn(b) == ActorGroup::ControlTurn::busy);
    assert(!group.finalized(a)); // Cannot finalize active lane.
    release.set_value();
    carrier.join();
    assert(group.run_control_turn(b) == ActorGroup::ControlTurn::executed);
    assert(group.finalized(a));
    assert(group.finalized(b));
}

static void control_handler_exceptions_retire_the_turn() {
    ActorGroup group(1);
    auto worker = group.register_spawn([] {});
    assert(group.mark_ready(worker));
    assert(group.enqueue_control(worker, [] { throw std::runtime_error("device failure"); }));
    require_throws([&] { (void)group.run_control_turn(worker); });
    assert(!group.has_pending_control(worker));
    assert(group.finalized(worker));
}

int main() {
    spawn_ref_is_pending_until_startup_and_done_is_independent();
    startup_failure_propagates_to_both_observers();
    cancelled_startup_failure_must_settle_done_exactly_once();
    concurrent_shutdown_and_failed_startup_do_not_strand_futures();
    close_fences_admission_and_only_requests_stop();
    stale_foreign_and_quota_cases_fail_closed();
    readiness_racing_close_cannot_resurrect_a_worker();
    gpu_device_callbacks_only_post_to_reserved_control_lane();
    group_control_lane_is_single_flight_even_on_different_carriers();
    control_handler_exceptions_retire_the_turn();
}
