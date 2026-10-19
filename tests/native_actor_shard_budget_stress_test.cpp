#include "oreslang/runtime/ActorShardColumns.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

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
    std::printf("native actor/VM shard budget contention: PASS attempts=%d success=%d\n",
                threads * per_thread, successes.load());
    return 0;
}
