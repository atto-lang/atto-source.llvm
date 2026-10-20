#include "oreslang/runtime/ActorShardColumns.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#if defined(NDEBUG)
#undef NDEBUG // Keep native proof assertions effective with -O2 -DNDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

using oreslang::runtime::ActorShardColumns;
using oreslang::runtime::ActorShardBudget;
using oreslang::dop::Layout;

struct Run {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef owner = {};
    Layout layout = Layout::SoA;
    std::size_t rows = 0;
    double nanoseconds = 0.0;
    std::unique_ptr<ActorShardBudget> actor_budget;
    ActorShardBudget vm_budget{64U * 1024U * 1024U};
    bool failed = false;
};

static void on_message(void* ctx, std::size_t actor, std::uint64_t iterations) {
    auto& run = *static_cast<Run*>(ctx);
    try {
        assert(actor == 0);
        ActorShardColumns shard(
            run.scheduler, run.owner, *run.actor_budget, run.vm_budget,
            run.layout, run.rows, 3,
            run.layout == Layout::AoSoA ? 8 : 1);
        for (std::size_t i = 0; i < run.rows; ++i) {
            shard.write(i, 0, static_cast<double>(i));
            shard.write(i, 1, 2);
        }
        for (int w = 0; w < 10; ++w) shard.integrate(0, 1, 1);
        auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t n = 0; n < iterations; ++n) {
            shard.integrate(0, 1, 1);
        }
        auto finish = std::chrono::steady_clock::now();
        run.nanoseconds = std::chrono::duration<double, std::nano>(finish - begin).count();
        if (shard.read(run.rows - 1, 0)
            != static_cast<double>(run.rows - 1 + 2 * (10 + iterations))) {
            run.failed = true;
        }
    } catch (...) {
        run.failed = true;
    }
}

int main() {
    for (std::size_t rows : {std::size_t(1000), std::size_t(10000), std::size_t(100000)}) {
        for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
            Run run;
            run.rows = rows;
            run.layout = layout;
            run.scheduler = ores_native_actor_scheduler_open(1, 2, 1, on_message, &run);
            assert(run.scheduler);
            run.owner = ores_native_actor_scheduler_spawn(run.scheduler, 0);
            run.actor_budget = std::make_unique<ActorShardBudget>(16U * 1024U * 1024U, run.scheduler, run.owner);
            assert(ores_native_actor_scheduler_send(run.scheduler, run.owner, 200));
            assert(ores_native_actor_scheduler_drain(run.scheduler));
            assert(!run.failed);
            std::printf("llvm-native rows=%zu layout=%d ns_per_row_update=%.3f\n",
                    rows, static_cast<int>(layout), run.nanoseconds / (200.0 * rows));
            ores_native_actor_scheduler_retire(run.scheduler);
            assert(ores_native_actor_scheduler_try_destroy(run.scheduler));
            assert(run.actor_budget->used_bytes() == 0);
            assert(run.vm_budget.used_bytes() == 0);
        }
    }
    return 0;
}
