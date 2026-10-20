#include "oreslang/runtime/ActorShardColumns.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <limits>
#include <stdexcept>

using oreslang::runtime::ActorShardBudget;

int main() {
    ActorShardBudget process(65536);
    ActorShardBudget actor(8192);
    ActorShardBudget tiny(3);
    assert(!tiny.try_reserve(4) && !tiny.try_reserve(0));
    assert(tiny.used_bytes() == 0);

    constexpr int threads = 8, per_thread = 100000;
    std::atomic<int> errors{0}, successes{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) {
        pool.emplace_back([&] {
            for (int i = 0; i < per_thread; i++) {
                constexpr std::uint64_t bytes = 512;
                if (!process.try_reserve(bytes)) continue;
                if (!actor.try_reserve(bytes)) {
                    process.release(bytes);
                    continue;
                }
                ++successes;
                if (actor.used_bytes() > actor.maximum_bytes()
                        || process.used_bytes() > process.maximum_bytes()) {
                    ++errors;
                }
                if ((i & 15) == 0) std::this_thread::yield();
                actor.release(bytes);
                process.release(bytes);
            }
        });
    }
    for (auto& t : pool) t.join();
    assert(errors == 0 && successes > 0);
    assert(process.used_bytes() == 0 && actor.used_bytes() == 0);

    // Retirement and reservation contend on one 64-bit atomic word.
    // Once retirement has completed, no new reserve may report success.
    ActorShardBudget racing(8192);
    std::atomic<bool> start{false}, retired{false};
    std::atomic<std::uint64_t> post_retire_success{0};
    pool.clear();
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < per_thread; ++i) {
                if (retired.load(std::memory_order_acquire)) {
                    if (racing.try_reserve(16)) {
                        post_retire_success.fetch_add(1);
                        racing.release(16);
                    }
                } else if (racing.try_reserve(16)) {
                    racing.release(16);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (int n = 0; n < 3000; ++n) std::this_thread::yield();
    racing.mark_retired_after_done();
    retired.store(true, std::memory_order_release);
    for (auto& t : pool) t.join();
    assert(racing.retired() && racing.used_bytes() == 0);
    assert(post_retire_success.load() == 0);
    assert(!racing.try_reserve(1));

    // Retire with an outstanding charge; the tombstone must survive refund.
    ActorShardBudget outstanding(4096);
    assert(outstanding.try_reserve(2048));
    outstanding.mark_retired_after_done();
    assert(outstanding.used_bytes() == 2048 && outstanding.retired());
    outstanding.release(2048);
    assert(outstanding.used_bytes() == 0 && !outstanding.try_reserve(1));

    bool too_large_rejected = false;
    try {
        ActorShardBudget impossible(std::uint64_t{1} << 63);
    } catch (const std::invalid_argument&) {
        too_large_rejected = true;
    }
    assert(too_large_rejected);

    std::printf("native actor/VM shard budget contention: PASS attempts=%d success=%d\n",
                threads * per_thread, successes.load());
    return 0;
}
