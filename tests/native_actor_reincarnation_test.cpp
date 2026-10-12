#include "oreslang/runtime/NativeActorScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;

static void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
static std::atomic<unsigned> messages{0};
static void on_message(void*, size_t, std::uint64_t) { ++messages; }
static bool valid(OresNativeActorRef ref) {
    return ref.scheduler_domain && ref.generation;
}
static void shutdown(OresNativeActorScheduler* s) {
    ores_native_actor_scheduler_retire(s);
    require(ores_native_actor_scheduler_drain(s)
            && ores_native_actor_scheduler_try_destroy(s), "scheduler shutdown failed");
}

int main() {
    auto* s = ores_native_actor_scheduler_open(2, 4, 2, on_message, nullptr);
    require(s != nullptr, "scheduler unavailable");
    auto parent = ores_native_actor_scheduler_spawn(s, 0);
    auto previous = ores_native_actor_scheduler_spawn_unpublished(s, 1);
    require(valid(parent) && valid(previous), "initial host spawn failed");
    constexpr int iterations = 10000;
    for (int i = 0; i < iterations; ++i) {
        require(!ores_native_actor_scheduler_recycle_unpublished_done(s, previous).scheduler_domain,
                "live hidden actor recycled before physical STOP");
        require(ores_native_actor_scheduler_stop(s, previous)
                && ores_native_actor_scheduler_wait_done(s, previous),
                "hidden candidate was not physically finalized");
        // Unpublished candidates never ran a callback or published PANIC.
        require(ores_native_actor_scheduler_poll_done(s, previous)
                && !ores_native_actor_scheduler_poll_published(s, previous),
                "candidate physical DONE barrier violated");
        const auto next = ores_native_actor_scheduler_recycle_unpublished_done(s, previous);
        require(valid(next) && next.scheduler_domain == previous.scheduler_domain
                    && next.actor_slot == previous.actor_slot
                    && next.generation == previous.generation + 1,
                "slot reuse did not increment an exact physical generation");
        require(!ores_native_actor_scheduler_poll_ready(s, previous)
                    && !ores_native_actor_scheduler_poll_done(s, previous)
                    && !ores_native_actor_scheduler_stop(s, previous)
                    && !ores_native_actor_scheduler_send(s, previous, 900),
                "stale physical handle accepted after generation reuse");
        require(!ores_native_actor_scheduler_submit(s, 1, 901),
                "legacy index-only submission bypassed reincarnation fencing");
        require(ores_native_actor_scheduler_poll_ready(s, next)
                    && !ores_native_actor_scheduler_poll_published(s, next)
                    && !ores_native_actor_scheduler_send(s, next, 902),
                "new candidate exposed message routing before RESTART commit");
        require(!ores_native_actor_scheduler_recycle_unpublished_done(s, previous)
                    .scheduler_domain,
                "stale cleanup reused a newer candidate");
        previous = next;
    }
    require(messages.load() == 0, "unpublished candidates executed user callback");

    OresNativeRestartLease lease{};
    require(ores_native_actor_scheduler_acquire_restart_lease(s, previous, &lease),
            "fresh generation did not acquire publication lease");
    require(!ores_native_actor_scheduler_recycle_unpublished_done(s, previous)
                .scheduler_domain,
            "same-thread recycle passed while lease was active");
    require(ores_native_actor_scheduler_publish_restart_lease(s, lease)
                && ores_native_actor_scheduler_release_restart_lease(s, lease),
            "generation-scoped publication lease failed");
    require(ores_native_actor_scheduler_send(s, previous, 903)
                && !ores_native_actor_scheduler_submit(s, 1, 904),
            "ref send and stale index send were not distinguished");
    require(ores_native_actor_scheduler_stop(s, previous)
                && ores_native_actor_scheduler_wait_done(s, previous),
            "published replacement was not physically finalized");
    require(!ores_native_actor_scheduler_recycle_unpublished_done(s, previous)
                .scheduler_domain,
            "published generation recycled without callback-aware ownership proof");
    require(messages.load() == 1, "unexpected user callback count");

    // Separate concurrency schedule: STOP and recycle must linearize under
    // the publication gate, never return a forged new generation.
    auto* race = ores_native_actor_scheduler_open(1, 2, 2, on_message, nullptr);
    require(race != nullptr, "race scheduler unavailable");
    auto old = ores_native_actor_scheduler_spawn_unpublished(race, 0);
    std::atomic<bool> start{false};
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        while (!start.load()) std::this_thread::yield();
        require(ores_native_actor_scheduler_stop(race, old), "concurrent stop failed");
        stopped.store(true);
    });
    start.store(true);
    auto attempt = ores_native_actor_scheduler_recycle_unpublished_done(race, old);
    stopper.join();
    if (!valid(attempt))
        attempt = ores_native_actor_scheduler_recycle_unpublished_done(race, old);
    require(stopped.load() && valid(attempt) && attempt.generation == 2,
            "racing physical STOP and recycle lost or duplicated generation");
    require(!ores_native_actor_scheduler_recycle_unpublished_done(race, old)
                .scheduler_domain,
            "duplicate recycling returned second physical generation");
    shutdown(race);
    shutdown(s);
    std::puts("NATIVE_ACTOR_REINCARNATION_PASS cycles=10000 stale=1 legacy=1 stop_race=1");
}
