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
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

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

/* Optional single-layout mode makes perf counters attributable to one
 * geometry instead of mixing AoS/SoA/AoSoA into an aggregate result.
 * Legacy no-argument smoke/benchmark behavior remains unchanged. */
static bool decimal(const char* text, std::uint64_t bound, std::uint64_t* out) {
    if (!text || !*text) return false;
    for (const char* c = text; *c; ++c)
        if (*c < '0' || *c > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno || !end || *end || !value || value > bound) return false;
    *out = static_cast<std::uint64_t>(value);
    return true;
}

int main(int argc, char** argv) {
    std::vector<std::size_t> sizes{1000, 10000, 100000};
    std::vector<Layout> layouts{Layout::AoS, Layout::SoA, Layout::AoSoA};
    std::uint64_t iterations = 200;
    if (argc != 1) {
        std::uint64_t requested_rows = 0;
        if (argc != 7 || std::strcmp(argv[1], "--layout") != 0
            || std::strcmp(argv[3], "--rows") != 0
            || std::strcmp(argv[5], "--iterations") != 0
            || !decimal(argv[4], 100000, &requested_rows)
            || !decimal(argv[6], 100000, &iterations)) {
            std::fputs("usage: native_actor_shard_layout_bench [--layout aos|soa|aosoa --rows 1..100000 --iterations 1..100000]\n", stderr);
            return 2;
        }
        Layout selected;
        if (std::strcmp(argv[2], "aos") == 0) selected = Layout::AoS;
        else if (std::strcmp(argv[2], "soa") == 0) selected = Layout::SoA;
        else if (std::strcmp(argv[2], "aosoa") == 0) selected = Layout::AoSoA;
        else {
            std::fputs("invalid data-oriented layout\n", stderr);
            return 2;
        }
        sizes.assign(1, static_cast<std::size_t>(requested_rows));
        layouts.assign(1, selected);
    }
    for (std::size_t rows : sizes) {
        for (auto layout : layouts) {
            Run run;
            run.rows = rows;
            run.layout = layout;
            run.scheduler = ores_native_actor_scheduler_open(1, 2, 1, on_message, &run);
            assert(run.scheduler);
            run.owner = ores_native_actor_scheduler_spawn(run.scheduler, 0);
            run.actor_budget = std::make_unique<ActorShardBudget>(16U * 1024U * 1024U, run.scheduler, run.owner);
            assert(ores_native_actor_scheduler_send(run.scheduler, run.owner, iterations));
            assert(ores_native_actor_scheduler_drain(run.scheduler));
            assert(!run.failed);
            std::printf("llvm-native rows=%zu layout=%d iterations=%llu ns_per_row_update=%.3f\n",
                    rows, static_cast<int>(layout),
                    static_cast<unsigned long long>(iterations),
                    run.nanoseconds / (static_cast<double>(iterations) * rows));
            ores_native_actor_scheduler_retire(run.scheduler);
            assert(ores_native_actor_scheduler_try_destroy(run.scheduler));
            assert(run.actor_budget->used_bytes() == 0);
            assert(run.vm_budget.used_bytes() == 0);
        }
    }
    return 0;
}
