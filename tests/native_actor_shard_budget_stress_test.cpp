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
                if (!ActorShardBudget::try_reserve_pair(actor, process, bytes)) {
                    continue;
                }
                ++successes;
                if (actor.used_bytes() > actor.maximum_bytes()
                        || process.used_bytes() > process.maximum_bytes()) {
                    ++errors;
                }
                if ((i & 15) == 0) std::this_thread::yield();
                ActorShardBudget::release_pair(actor, process, bytes);
            }
        });
    }
    for (auto& t : pool) t.join();
    assert(errors == 0 && successes > 0);
    assert(process.used_bytes() == 0 && actor.used_bytes() == 0);

    // Pair admission is all-or-nothing regardless of which ledger is tight.
    ActorShardBudget tight_actor(256);
    ActorShardBudget roomy_vm(4096);
    assert(!ActorShardBudget::try_reserve_pair(tight_actor, roomy_vm, 512));
    assert(tight_actor.used_bytes() == 0);
    assert(roomy_vm.used_bytes() == 0);

    ActorShardBudget roomy_actor(4096);
    ActorShardBudget tight_vm(256);
    assert(!ActorShardBudget::try_reserve_pair(roomy_actor, tight_vm, 512));
    assert(roomy_actor.used_bytes() == 0);
    assert(tight_vm.used_bytes() == 0);

    ActorShardBudget retired_pair_actor(4096);
    ActorShardBudget retired_pair_vm(4096);
    retired_pair_actor.mark_retired_after_done();
    assert(!ActorShardBudget::try_reserve_pair(
            retired_pair_actor, retired_pair_vm, 512));
    assert(retired_pair_actor.used_bytes() == 0);
    assert(retired_pair_vm.used_bytes() == 0);

    // Retirement and reservation contend on the same ledger lock.
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

    // Pair admission must serialize with actor retirement too. Once the
    // retirement operation has completed, no paired actor/VM reservation may
    // commit and the VM ledger must never retain a partial charge.
    ActorShardBudget paired_actor(8192);
    ActorShardBudget paired_vm(65536);
    std::atomic<bool> paired_start{false}, paired_retired{false};
    std::atomic<std::uint64_t> paired_post_retire_success{0};
    pool.clear();
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            while (!paired_start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < per_thread; ++i) {
                const bool after =
                        paired_retired.load(std::memory_order_acquire);
                if (ActorShardBudget::try_reserve_pair(
                        paired_actor, paired_vm, 16)) {
                    if (after) paired_post_retire_success.fetch_add(1);
                    ActorShardBudget::release_pair(
                            paired_actor, paired_vm, 16);
                }
            }
        });
    }
    paired_start.store(true, std::memory_order_release);
    for (int n = 0; n < 3000; ++n) std::this_thread::yield();
    paired_actor.mark_retired_after_done();
    paired_retired.store(true, std::memory_order_release);
    for (auto& t : pool) t.join();
    assert(paired_actor.retired());
    assert(paired_actor.used_bytes() == 0);
    assert(paired_vm.used_bytes() == 0);
    assert(paired_post_retire_success.load() == 0);
    assert(!ActorShardBudget::try_reserve_pair(
            paired_actor, paired_vm, 16));
    assert(paired_vm.used_bytes() == 0);

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
