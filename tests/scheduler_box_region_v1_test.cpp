#include "oreslang/runtime/SchedulerBoxRegionV1.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <type_traits>

using oreslang::runtime::native_ownership_v1::AtomicScalar;
using oreslang::runtime::native_ownership_v1::AtomicStatus;
using oreslang::runtime::native_ownership_v1::SchedulerBoxRegionV1;

#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

struct Counter {
    AtomicScalar<int> count;
    std::atomic<int>* drops;
    explicit Counter(std::atomic<int>* d) noexcept : count(0), drops(d) {}
    ~Counter() noexcept { drops->fetch_add(1, std::memory_order_relaxed); }
};
using Region = SchedulerBoxRegionV1<Counter, 1>;
static_assert(!std::is_copy_constructible_v<Region>);
static_assert(!std::is_move_constructible_v<Region>);

struct State {
    std::atomic<Region*> region{nullptr};
    std::atomic<int> initialized{0}, processed{0}, rejected{0}, drops{0};
    std::atomic<int> total{0};
};
static void receive(void* raw, std::size_t actor_index, std::uint64_t payload) {
    auto& state = *static_cast<State*>(raw);
    Region* region = state.region.load(std::memory_order_acquire);
    if (!region) { state.rejected.fetch_add(1); return; }
    if (actor_index == 1) {
        // A different live actor cannot access the first actor's Box.
        if (region->with_at(0, [](Counter&) {}) ||
            region->emplace_at(0, &state.drops) || region->drop_at(0))
            state.rejected.fetch_add(1);
        state.processed.fetch_add(1);
        return;
    }
    if (payload == 0) {
        if (!region->emplace_at(0, &state.drops) ||
            region->emplace_at(0, &state.drops))
            state.rejected.fetch_add(1);
        state.initialized.store(1, std::memory_order_release);
        state.processed.fetch_add(1);
        return;
    }
    if (payload != 1) { state.rejected.fetch_add(1); return; }
    if (!region->with_at(0, [&](Counter& counter) {
        if (region->drop_at(0) || region->with_at(0, [](Counter&) {}))
            state.rejected.fetch_add(1);
        for (int k = 0; k < 10000; ++k) {
            int previous = 0;
            if (counter.count.fetch_add(1, &previous) != AtomicStatus::Ok)
                state.rejected.fetch_add(1);
        }
        int observed = -1;
        if (counter.count.load(&observed) != AtomicStatus::Ok)
            state.rejected.fetch_add(1);
        state.total.store(observed, std::memory_order_release);
    })) state.rejected.fetch_add(1);
    state.processed.fetch_add(1, std::memory_order_release);
}
int main() {
    State state;
    auto* scheduler = ores_native_actor_scheduler_open(2, 128, 4, &receive, &state);
    REQUIRE(scheduler);
    const auto actor = ores_native_actor_scheduler_spawn(scheduler, 0);
    const auto foreign = ores_native_actor_scheduler_spawn(scheduler, 1);
    REQUIRE(actor.scheduler_domain && actor.generation);
    REQUIRE(foreign.scheduler_domain == actor.scheduler_domain && foreign.generation);
    {
        Region region(scheduler, actor);
        state.region.store(&region, std::memory_order_release);
        REQUIRE(!region.emplace_at(0, &state.drops)); // no actor turn
        REQUIRE(!region.with_at(0, [](Counter&) {}));
        REQUIRE(!region.drop_at(0));
        REQUIRE(ores_native_actor_scheduler_try_send(scheduler, actor, 0)
                == ORES_NATIVE_SEND_ACCEPTED);
        // Parallel producers must still pass through real mailbox admission.
        std::thread producers[4];
        for (auto& t : producers) t = std::thread([&] {
            for (int n = 0; n < 25; ++n)
                if (ores_native_actor_scheduler_try_send(scheduler, actor, 1)
                        != ORES_NATIVE_SEND_ACCEPTED)
                    state.rejected.fetch_add(1);
        });
        for (auto& t : producers) t.join();
        REQUIRE(ores_native_actor_scheduler_try_send(scheduler, foreign, 2)
                == ORES_NATIVE_SEND_ACCEPTED);
        REQUIRE(ores_native_actor_scheduler_drain(scheduler));
        REQUIRE(state.initialized.load(std::memory_order_acquire) == 1);
        REQUIRE(state.processed.load(std::memory_order_acquire) == 102);
        REQUIRE(state.total.load(std::memory_order_acquire) == 1000000);
        REQUIRE(state.rejected.load() == 0);
        REQUIRE(!region.with_at(0, [](Counter&) {})); // no foreign/root access
        REQUIRE(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
        REQUIRE(ores_native_actor_scheduler_stop_sealed(scheduler, foreign));
        REQUIRE(region.retire_after_done()); // exact STOP and DONE fence
        REQUIRE(region.is_retired());
        REQUIRE(state.drops.load() == 1);
        REQUIRE(!region.retire_after_done()); // no repeated teardown
    }
    ores_native_actor_scheduler_retire(scheduler);
    REQUIRE(ores_native_actor_scheduler_drain(scheduler));
    REQUIRE(ores_native_actor_scheduler_try_destroy(scheduler));
    std::puts("ATTO_SCHEDULER_BOX_REGION_V1_PASS");
}
