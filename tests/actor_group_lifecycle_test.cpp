#include "oreslang/runtime/ActorGroup.hpp"

#include <atomic>
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

int main() {
    spawn_ref_is_pending_until_startup_and_done_is_independent();
    startup_failure_propagates_to_both_observers();
    close_fences_admission_and_only_requests_stop();
    stale_foreign_and_quota_cases_fail_closed();
    readiness_racing_close_cannot_resurrect_a_worker();
}
